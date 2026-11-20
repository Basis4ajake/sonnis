#define _GNU_SOURCE // strcasestr
#include "ui.h"
#include "albumart.h"
#include "controller.h"
#include "settings.h"
#include "ui_theme.h"
#include "ui_font.h"
#include "netlog.h"
#include "logserver.h"
#include "xml_util.h"
#include "ozzy.h"
#include "sound.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <citro2d.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <math.h>
#include <ctype.h>

#define TOP_W 400
#define BOT_W 320
#define SCR_H 240
#define Z 0.5f

// 3DS system-font button glyphs (private-use area).
#define G_A ""
#define G_B ""
#define G_X ""
#define G_Y ""
#define G_L ""
#define G_R ""
#define G_UP ""
#define G_DOWN ""
#define G_LEFT ""
#define G_RIGHT ""

typedef enum { SCR_NOW, SCR_BROWSE, SCR_ROOMS, SCR_SETTINGS, SCR_DIAG, SCR_NETLOG } screen_t;

static C3D_RenderTarget *top, *bot;

static C3D_Tex art_tex;
static Tex3DS_SubTexture art_sub = { ART_SIZE, ART_SIZE, 0.0f, 1.0f, 1.0f, 0.0f };
static C2D_Image art_img = { &art_tex, &art_sub };
static bool art_valid;

// Vinyl: a CPU-rotated, circle-cropped copy of the art (citro2d can only
// rotate whole squares, whose corners would stick out past the record).
// Two textures so the CPU never writes the one the GPU is still drawing.
static C3D_Tex vinyl_tex[2];
static Tex3DS_SubTexture vinyl_sub = { ART_SIZE, ART_SIZE, 0.0f, 1.0f, 1.0f, 0.0f };
static int vinyl_buf;
static u32 *art_copy;      // UI-thread copy of the art pixels (0xRRGGBBAA)
static bool vinyl_dirty;   // art changed since the last rotation
static int art_gen_seen = -1;

static screen_t screen = SCR_NOW;
static u32 frame_no;

// ---- browse navigation ----------------------------------------------------

typedef struct {
	char id[160];
	char title[64];
	int cursor;
	float scroll;
} bframe_t;

#define MAX_DEPTH 12
static bframe_t bstack[MAX_DEPTH]; // [0] = root menu
static int bdepth;

typedef struct { const char *title, *subtitle, *id; } root_entry_t;
static const root_entry_t ROOT[] = {
	{ "Sonos Favorites", "Saved stations, albums and playlists", LIB_ID_FAVORITES },
	{ "Sonos Playlists", "Playlists saved on your system", LIB_ID_PLAYLISTS },
	{ "Queue", "Up next in this room", LIB_ID_QUEUE },
	{ "Recently Played", "Started from this 2DS", LIB_ID_HISTORY },
	{ "Artists", "Music library", LIB_ID_ARTISTS },
	{ "Albums", "Music library", LIB_ID_ALBUMS },
	{ "Tracks", "Music library", LIB_ID_TRACKS },
	{ "Genres", "Music library", LIB_ID_GENRES },
	{ "Radio Stations", "My Radio Stations (local)", LIB_ID_RADIO },
	{ "Music Services", "Spotify, Apple Music... (needs cloud)", LIB_ID_SERVICES },
};
#define ROOT_COUNT ((int)(sizeof ROOT / sizeof *ROOT))

static int rooms_cursor;
static float rooms_scroll;
static int set_cursor;
static float set_scroll;
static bool settings_dirty;
static bool quit;

// ==== drawing primitives ======================================================

static float measure(const char *s, float px)
{
	char vb[256];
	return font_measure(vox(s, vb, sizeof vb), px);
}

static size_t utf8_back(const char *s, size_t n)
{
	while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
	return n;
}

// Copies s into out, shortened with "..." to fit maxw.
static void fit(const char *s, float px, float maxw, char *out, size_t cap)
{
	str_copy(out, s, cap);
	if (maxw <= 0 || font_measure(out, px) <= maxw) return;
	size_t lo = 0, hi = strlen(out);
	char tmp[256];
	while (lo < hi) { // largest prefix that fits with the ellipsis
		size_t mid = utf8_back(out, (lo + hi + 1) / 2);
		if (mid <= lo) { hi = lo; break; }
		snprintf(tmp, sizeof tmp, "%.*s...", (int)mid, out);
		if (font_measure(tmp, px) <= maxw) lo = mid; else hi = mid - 1;
	}
	lo = utf8_back(out, lo);
	while (lo > 0 && out[lo - 1] == ' ') lo--;
	snprintf(tmp, sizeof tmp, "%.*s...", (int)lo, out);
	str_copy(out, tmp, cap);
}

enum { AL_LEFT, AL_CENTER, AL_RIGHT };

static float text(const char *s, float x, float y, float px, u32 color, int align, float maxw)
{
	char vb[256], buf[256];
	fit(vox(s, vb, sizeof vb), px, maxw, buf, sizeof buf);
	float w = font_measure(buf, px);
	if (align == AL_CENTER) x -= w / 2;
	else if (align == AL_RIGHT) x -= w;
	font_draw(buf, x, y, px, color);
	return w;
}

// Two-line word wrap; second line is ellipsized. Returns lines used.
static int text_wrap2(const char *s, float x, float y, float px, float lh, u32 color, float maxw)
{
	if (measure(s, px) <= maxw) { text(s, x, y, px, color, AL_LEFT, 0); return 1; }
	size_t len = strlen(s), best = 0;
	char tmp[256];
	for (size_t i = 1; i < len && i < sizeof tmp; i++) {
		if (s[i] != ' ') continue;
		snprintf(tmp, sizeof tmp, "%.*s", (int)i, s);
		if (measure(tmp, px) <= maxw) best = i; else break;
	}
	if (best == 0) { text(s, x, y, px, color, AL_LEFT, maxw); return 1; }
	snprintf(tmp, sizeof tmp, "%.*s", (int)best, s);
	text(tmp, x, y, px, color, AL_LEFT, 0);
	text(s + best + 1, x, y + lh, px, color, AL_LEFT, maxw);
	return 2;
}

static void rect(float x, float y, float w, float h, u32 c) { C2D_DrawRectSolid(x, y, Z, w, h, c); }

static void rrect(float x, float y, float w, float h, float r, u32 c)
{
	if (r * 2 > h) r = h / 2;
	if (r * 2 > w) r = w / 2;
	rect(x + r, y, w - 2 * r, h, c);
	rect(x, y + r, r, h - 2 * r, c);
	rect(x + w - r, y + r, r, h - 2 * r, c);
	C2D_DrawCircleSolid(x + r, y + r, Z, r, c);
	C2D_DrawCircleSolid(x + w - r, y + r, Z, r, c);
	C2D_DrawCircleSolid(x + r, y + h - r, Z, r, c);
	C2D_DrawCircleSolid(x + w - r, y + h - r, Z, r, c);
}

static void tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 c)
{
	C2D_DrawTriangle(x0, y0, c, x1, y1, c, x2, y2, c, Z);
}

static void icon_play(float cx, float cy, float s, u32 c) { tri(cx - s * 0.35f, cy - s * 0.5f, cx - s * 0.35f, cy + s * 0.5f, cx + s * 0.55f, cy, c); }
static void icon_pause(float cx, float cy, float s, u32 c)
{
	rect(cx - s * 0.4f, cy - s * 0.5f, s * 0.28f, s, c);
	rect(cx + s * 0.12f, cy - s * 0.5f, s * 0.28f, s, c);
}
static void icon_next(float cx, float cy, float s, u32 c)
{
	tri(cx - s * 0.45f, cy - s * 0.45f, cx - s * 0.45f, cy + s * 0.45f, cx + s * 0.25f, cy, c);
	rect(cx + s * 0.25f, cy - s * 0.45f, s * 0.15f, s * 0.9f, c);
}
static void icon_prev(float cx, float cy, float s, u32 c)
{
	tri(cx + s * 0.45f, cy - s * 0.45f, cx + s * 0.45f, cy + s * 0.45f, cx - s * 0.25f, cy, c);
	rect(cx - s * 0.4f, cy - s * 0.45f, s * 0.15f, s * 0.9f, c);
}
static void icon_speaker(float cx, float cy, float s, bool muted, u32 c)
{
	rect(cx - s * 0.5f, cy - s * 0.18f, s * 0.25f, s * 0.36f, c);
	tri(cx - s * 0.3f, cy, cx + s * 0.05f, cy - s * 0.45f, cx + s * 0.05f, cy + s * 0.45f, c);
	if (muted) {
		C2D_DrawLine(cx + s * 0.2f, cy - s * 0.2f, c, cx + s * 0.5f, cy + s * 0.2f, c, 2, Z);
		C2D_DrawLine(cx + s * 0.5f, cy - s * 0.2f, c, cx + s * 0.2f, cy + s * 0.2f, c, 2, Z);
	} else {
		rect(cx + s * 0.2f, cy - s * 0.15f, 2, s * 0.3f, c);
		rect(cx + s * 0.38f, cy - s * 0.3f, 2, s * 0.6f, c);
	}
}
static void icon_note(float cx, float cy, float s, u32 c)
{
	C2D_DrawCircleSolid(cx - s * 0.2f, cy + s * 0.3f, Z, s * 0.18f, c);
	rect(cx - s * 0.06f, cy - s * 0.45f, s * 0.08f, s * 0.75f, c);
	tri(cx - s * 0.02f, cy - s * 0.45f, cx + s * 0.35f, cy - s * 0.3f, cx - s * 0.02f, cy - s * 0.15f, c);
}
static void icon_pin(float cx, float cy, float s, u32 c) // small diamond
{
	tri(cx, cy - s / 2, cx + s / 2, cy, cx - s / 2, cy, c);
	tri(cx - s / 2, cy, cx + s / 2, cy, cx, cy + s / 2, c);
}
static void wifi_bars(float x, float y, int strength, bool up)
{
	for (int i = 0; i < 3; i++) {
		float h = 3 + i * 3;
		rect(x + i * 5, y + 9 - h, 3, h, up && i < strength ? C.text : C.line);
	}
}

// ==== Deadhead theme effects =================================================

static float fx_time(void) { return frame_no / 60.0f; }

// Which animated background is showing. The theme can ask for tie-dye, and
// the song playing can override it (see track_fx).
typedef enum { FX_NONE, FX_TIEDYE, FX_LASER, FX_SABBATH } fx_mode_t;
static fx_mode_t fx_mode;
static float fx_phase; // effect clock: runs faster while music plays

// The bottom screen sits under the middle of the top one. Effects that span
// both screens are drawn in "world" coordinates (top screen = origin) and
// shifted by this much on the bottom screen; the GPU clips the rest.
#define BOT_OX 40.0f
#define BOT_OY 260.0f

// Picks a special effect for the current song, or FX_NONE to use the theme's.
static fx_mode_t track_fx(const track_info_t *t)
{
	// The Biscuits family gets lasers; the extended Dead family (and its San
	// Francisco cousins) gets tie-dye. Checked against artist, album and
	// title, because live tapes and streams often leave the artist empty.
	static const char *const SABBATH[] = {
		"ozzy", "black sabbath", "sabbath", "tony iommi", "geezer butler",
		"bill ward", "randy rhoads", "zakk wylde", "heaven & hell",
		"heaven and hell", "dio", "jake e. lee", "osbourne",
	};
	static const char *const LASER[] = {
		"disco biscuits", "tractorbeam", "conspirator", "jon gutwillig",
	};
	static const char *const DEAD[] = {
		"grateful dead", "dead & company", "dead and company", "jerry garcia",
		"phil lesh", "bob weir", "ratdog", "furthur", "the other ones",
		"mickey hart", "bill kreutzmann", "billy & the kids", "planet drum",
		"jefferson airplane", "jefferson starship", "hot tuna",
		"quicksilver messenger", "new riders of the purple sage",
		"legion of mary", "old & in the way", "old and in the way",
		"keith & donna", "dark star orchestra", "almost dead", "grateful shred",
		"wolf bros", "dick's picks", "dave's picks", "road trips vol", "europe '72",
	};
	const char *fields[] = { t->artist, t->album, t->title };
	for (int f = 0; f < 3; f++) {
		if (!fields[f][0]) continue;
		for (size_t i = 0; i < sizeof SABBATH / sizeof *SABBATH; i++) {
			const char *m = strcasestr(fields[f], SABBATH[i]);
			// "dio" only as a whole word (not "radio", "studio", "audio").
			if (m && !strcmp(SABBATH[i], "dio") &&
			    ((m > fields[f] && isalpha((unsigned char)m[-1])) || isalpha((unsigned char)m[3]))) m = NULL;
			if (m) return FX_SABBATH;
		}
		for (size_t i = 0; i < sizeof LASER / sizeof *LASER; i++)
			if (strcasestr(fields[f], LASER[i])) return FX_LASER;
		for (size_t i = 0; i < sizeof DEAD / sizeof *DEAD; i++)
			if (strcasestr(fields[f], DEAD[i])) return FX_TIEDYE;
	}
	return FX_NONE;
}

// Effect color at position u (wraps at 1): the full rainbow for tie-dye,
// a neon green -> cyan -> magenta sweep for the laser show.
static u32 fx_color(float u, float sat, float val)
{
	if (fx_mode == FX_LASER) {
		float w = 0.5f + 0.5f * sinf(u * 6.2831853f);
		return theme_hue(0.33f + 0.5f * w, 0.9f, 1.0f, 0xFF);
	}
	if (fx_mode == FX_SABBATH) { // blood red -> pumpkin orange -> witchy purple
		float w = 0.5f + 0.5f * sinf(u * 6.2831853f);
		return theme_hue(0.80f + 0.30f * w, 0.9f, 1.0f, 0xFF);
	}
	return theme_hue(u, sat, val, 0xFF);
}

// A tie-dye spiral: rings of quads whose hue depends on angle AND radius, so
// the colors twist into arms; shifting the hue over time makes it swirl.
// Per-vertex colors let the GPU blend smoothly between the sample points.
static void draw_tiedye(float cx, float cy, float radius, float t)
{
	enum { RINGS = 10, SEGS = 40 };
	const float TAU = 6.2831853f;
	static float cs[SEGS + 1], sn[SEGS + 1];
	static bool init;
	if (!init) { // trig is costly on the 268 MHz CPU; angles never change
		for (int s = 0; s <= SEGS; s++) { cs[s] = cosf(TAU * s / SEGS); sn[s] = sinf(TAU * s / SEGS); }
		init = true;
	}
	// Hue per (segment, ring) grid point: 2 spiral arms, twist with radius,
	// slow rotation over time. Computed once per vertex, shared by 4 quads.
	static u32 hue[RINGS + 1][SEGS + 1];
	static float hue_t = -1;
	if (t != hue_t) // both screens draw it with the same t: compute once
	for (int r = 0; r <= RINGS; r++)
		for (int s = 0; s <= SEGS; s++)
			hue[r][s] = theme_hue((float)s / SEGS * 2.0f + (float)r / RINGS * 1.6f - t * 0.05f, 0.85f, 0.9f, 0xFF);
	hue_t = t;

	for (int r = 0; r < RINGS; r++) {
		float r0 = radius * r / RINGS, r1 = radius * (r + 1) / RINGS;
		for (int s = 0; s < SEGS; s++) {
			float x00 = cx + cs[s] * r0, y00 = cy + sn[s] * r0;
			float x01 = cx + cs[s + 1] * r0, y01 = cy + sn[s + 1] * r0;
			float x10 = cx + cs[s] * r1, y10 = cy + sn[s] * r1;
			float x11 = cx + cs[s + 1] * r1, y11 = cy + sn[s + 1] * r1;
			C2D_DrawTriangle(x00, y00, hue[r][s], x10, y10, hue[r + 1][s], x11, y11, hue[r + 1][s + 1], Z);
			C2D_DrawTriangle(x00, y00, hue[r][s], x11, y11, hue[r + 1][s + 1], x01, y01, hue[r][s + 1], Z);
		}
	}
}

// Horizontal rainbow bar (progress, volume).
static void rainbow_bar(float x, float y, float w, float h, float t)
{
	const int seg = 10;
	for (float sx = 0; sx < w; sx += seg) {
		float sw = sx + seg > w ? w - sx : seg;
		u32 a = fx_color((x + sx) / 300.0f - t * 0.1f, 0.75f, 1.0f);
		u32 b = fx_color((x + sx + sw) / 300.0f - t * 0.1f, 0.75f, 1.0f);
		C2D_DrawRectangle(x + sx, y, Z, sw, h, a, b, a, b);
	}
}

// Rainbow halo ring behind a circular button.
static void rainbow_halo(float cx, float cy, float r, float t)
{
	enum { SEGS = 36 };
	const float TAU = 6.2831853f;
	for (int s = 0; s < SEGS; s++) {
		float a0 = TAU * s / SEGS, a1 = TAU * (s + 1) / SEGS;
		u32 c0 = fx_color((float)s / SEGS + t * 0.15f, 0.8f, 1.0f);
		u32 c1 = fx_color((float)(s + 1) / SEGS + t * 0.15f, 0.8f, 1.0f);
		C2D_DrawTriangle(cx, cy, c0, cx + cosf(a0) * r, cy + sinf(a0) * r, c0, cx + cosf(a1) * r, cy + sinf(a1) * r, c1, Z);
	}
}

// ==== Space disco laser show =================================================

static u32 rgba(u8 r, u8 g, u8 b, u8 a) { return C2D_Color32(r, g, b, a); }

// Deep-space gradient, twinkling stars, a scrolling neon grid floor and
// sweeping laser beams fired from a mirror ball. t advances faster while
// the band is playing.
// Beams from the mirror ball (world 200,22), long enough to reach down
// across the bottom screen too. (ox, oy) = this screen's world offset.
static void laser_beams(float t, float ox, float oy)
{
	const float TAU = 6.2831853f, bx = TOP_W / 2.0f - ox, by = 22 - oy;
	for (int i = 0; i < 7; i++) {
		float a = TAU / 4 + sinf(t * (0.6f + i * 0.13f) + i * 1.9f) * 1.0f;
		float ex = bx + cosf(a) * 560, ey = by + sinf(a) * 560;
		float nx = -sinf(a) * 6, ny = cosf(a) * 6; // half-width at the far end
		u32 c = theme_hue(0.33f + 0.5f * (0.5f + 0.5f * sinf(t * 0.7f + i)), 0.9f, 1.0f, 0xD0);
		u32 fade = (c & 0x00FFFFFF) | 0x18000000;
		C2D_DrawTriangle(bx, by, c, ex + nx, ey + ny, fade, ex - nx, ey - ny, fade, Z);
	}
}

static void draw_lasers(float t)
{
	const float TAU = 6.2831853f, HZ = 150, VX = TOP_W / 2.0f;
	C2D_DrawRectangle(0, 0, Z, TOP_W, SCR_H, rgba(4, 2, 18, 255), rgba(4, 2, 18, 255),
	                  rgba(40, 6, 60, 255), rgba(40, 6, 60, 255));

	// Stars: fixed pseudo-random spots (integer hash), each twinkling.
	for (u32 i = 0; i < 70; i++) {
		u32 h = i * 2654435761u;
		float x = (h % 400), y = ((h >> 9) % 140);
		float tw = 0.5f + 0.5f * sinf(t * 1.7f + i * 1.3f);
		rect(x, y, 1 + (i % 3 == 0), 1, rgba(255, 255, 255, (u8)(60 + 180 * tw)));
	}

	// Grid floor: lines rush toward the viewer (perspective: y ~ depth^2).
	for (int k = 0; k < 9; k++) {
		float d = fmodf(k + t * 1.2f, 9.0f) / 9.0f;
		float y = HZ + (SCR_H - HZ) * d * d;
		C2D_DrawLine(0, y, rgba(255, 40, 200, (u8)(40 + 200 * d)), TOP_W, y,
		             rgba(255, 40, 200, (u8)(40 + 200 * d)), 1 + d, Z);
	}
	for (int i = -8; i <= 8; i++)
		C2D_DrawLine(VX + i * 6, HZ, rgba(0, 220, 255, 40), VX + i * 70, SCR_H, rgba(0, 220, 255, 200), 1, Z);

	// Laser beams from the mirror ball: thin wedges, bright at the source,
	// fading out, sweeping back and forth across the room.
	float bx = VX, by = 22;
	laser_beams(t, 0, 0);

	// Mirror ball with glinting facets.
	C2D_DrawCircleSolid(bx, by, Z, 15, rgba(170, 170, 190, 255));
	for (int i = 0; i < 12; i++) {
		float a = TAU * i / 12 + t * 0.8f;
		float tw = 0.5f + 0.5f * sinf(t * 5 + i * 2.1f);
		C2D_DrawCircleSolid(bx + cosf(a) * 9, by + sinf(a) * 5 + 1, Z, 2, rgba(255, 255, 255, (u8)(80 + 170 * tw)));
	}
}


// Bottom screen of the laser show: a light-up disco floor seen from above,
// with the beams from the top screen landing on it.
static void draw_lasers_bottom(float t)
{
	rect(0, 0, BOT_W, SCR_H, rgba(6, 2, 20, 255));
	enum { TS = 40 };
	for (int ty = 0; ty < SCR_H / TS; ty++)
		for (int tx = 0; tx < BOT_W / TS; tx++) {
			u32 h = (u32)(tx * 7 + ty * 13) * 2654435761u;
			// Each tile pulses on its own beat; neighbours differ in color.
			float beat = 0.5f + 0.5f * sinf(t * 3.1f + (h >> 20) % 7);
			u32 c = theme_hue(0.33f + 0.5f * (((h >> 8) % 3) / 2.0f), 0.85f, 0.25f + 0.65f * beat * beat, 0xFF);
			rrect(tx * TS + 2, ty * TS + 2, TS - 4, TS - 4, 4, c);
		}
	laser_beams(t, BOT_OX, BOT_OY);
	// Sparkles thrown off the mirror ball.
	for (u32 i = 0; i < 24; i++) {
		u32 h = (i + 77) * 2246822519u;
		float tw = sinf(t * 2.3f + i * 2.7f);
		if (tw < 0.6f) continue;
		float x = h % BOT_W, y = (h >> 11) % SCR_H, k = (tw - 0.6f) * 10;
		rect(x - k, y, 2 * k + 1, 1, rgba(255, 255, 255, 220));
		rect(x, y - k, 1, 2 * k + 1, rgba(255, 255, 255, 220));
	}
}

// ==== Black Sabbath Halloween =================================================

// Lightning: a flash that fades over ~1/3 s, with one jagged bolt. Shared by
// both screens so they flash together.
static struct { float level; float x[9], y[9]; float timer; } bolt;

static void sabbath_tick(bool playing)
{
	if (bolt.level > 0) bolt.level -= 1.0f / 20;
	bolt.timer -= 1.0f / 60;
	if (bolt.timer > 0) return;
	bolt.timer = (playing ? 2.5f : 7.0f) + (rand() % 400) / 100.0f;
	bolt.level = 1;
	float x = 60 + rand() % 280, y = 0;
	for (int i = 0; i < 9; i++) { // from the clouds to the ground
		bolt.x[i] = x; bolt.y[i] = y;
		x += (rand() % 50) - 25;
		y += 24 + rand() % 12;
	}
}

static void draw_bolt(float ox, float oy)
{
	if (bolt.level <= 0) return;
	u8 a = (u8)(255 * bolt.level);
	for (int i = 0; i < 8; i++) {
		C2D_DrawLine(bolt.x[i] - ox, bolt.y[i] - oy, rgba(190, 160, 255, a / 2), bolt.x[i + 1] - ox, bolt.y[i + 1] - oy, rgba(190, 160, 255, a / 2), 5, Z);
		C2D_DrawLine(bolt.x[i] - ox, bolt.y[i] - oy, rgba(255, 255, 255, a), bolt.x[i + 1] - ox, bolt.y[i + 1] - oy, rgba(255, 255, 255, a), 1.5f, Z);
	}
}

// A bat with flapping wings. World coordinates.
static void draw_bat(float x, float y, float flap, float s)
{
	u32 c = rgba(8, 4, 12, 255);
	float up = sinf(flap) * 6 * s;
	tri(x, y, x - 13 * s, y - up, x - 5 * s, y + 3 * s, c);
	tri(x, y, x + 13 * s, y - up, x + 5 * s, y + 3 * s, c);
	tri(x - 13 * s, y - up, x - 8 * s, y + 2 * s - up / 2, x - 5 * s, y + 3 * s, c);
	tri(x + 13 * s, y - up, x + 8 * s, y + 2 * s - up / 2, x + 5 * s, y + 3 * s, c);
	C2D_DrawEllipseSolid(x - 3 * s, y - 2 * s, Z, 6 * s, 6 * s, c);
	tri(x - 2.5f * s, y - 2 * s, x - 1.5f * s, y - 5 * s, x - 0.5f * s, y - 2 * s, c); // ears
	tri(x + 0.5f * s, y - 2 * s, x + 1.5f * s, y - 5 * s, x + 2.5f * s, y - 2 * s, c);
}

// Bats and rain live in world space so they cross from one screen to the other.
static void sabbath_world(float t, float ox, float oy)
{
	for (u32 i = 0; i < 70; i++) { // rain
		u32 h = (i + 3) * 2654435761u;
		float y = fmodf((h >> 7) % 500 + t * 260, 500) - oy;
		float x = (h % 480) - 40 - ox + (y + oy) * 0.12f;
		C2D_DrawLine(x, y, rgba(150, 150, 200, 0), x + 2, y + 12, rgba(170, 170, 220, 110), 1, Z);
	}
	for (int i = 0; i < 6; i++) { // bats swoop across both screens
		float p = fmodf(t * (0.06f + i * 0.013f) + i * 0.37f, 1.0f);
		float x = -30 + p * 470 - ox;
		float y = 60 + i * 70 + sinf(t * 1.3f + i * 2) * 25 - oy;
		draw_bat(x, y, t * 14 + i, i % 2 ? 0.8f : 1.1f);
	}
	draw_bolt(ox, oy);
}

// Top: stormy night, a huge moon, a dead tree and a graveyard on the hill.
static void draw_sabbath_top(float t)
{
	C2D_DrawRectangle(0, 0, Z, TOP_W, SCR_H, rgba(14, 4, 20, 255), rgba(14, 4, 20, 255),
	                  rgba(60, 10, 30, 255), rgba(60, 10, 30, 255));
	// Moon with a blood-orange glow.
	for (int g = 4; g >= 1; g--) C2D_DrawCircleSolid(318, 62, Z, 34 + g * 9, rgba(255, 120, 40, 14));
	C2D_DrawCircleSolid(318, 62, Z, 34, rgba(250, 236, 200, 255));
	C2D_DrawCircleSolid(306, 52, Z, 7, rgba(222, 206, 170, 255));
	C2D_DrawCircleSolid(328, 74, Z, 5, rgba(222, 206, 170, 255));
	C2D_DrawCircleSolid(330, 50, Z, 3, rgba(222, 206, 170, 255));
	// Drifting clouds across the moon.
	for (int i = 0; i < 4; i++) {
		float x = fmodf(t * (8 + i * 3) + i * 140, 560) - 80;
		C2D_DrawEllipseSolid(x, 30 + i * 22, Z, 120, 22, rgba(30, 14, 40, 150));
	}
	// Hill, tombstones, crosses, dead tree (silhouettes).
	u32 ink = rgba(6, 2, 10, 255);
	C2D_DrawEllipseSolid(-60, 196, Z, 520, 120, ink);
	for (int i = 0; i < 6; i++) {
		float x = 30 + i * 62, y = 206 - (i % 3) * 5;
		if (i % 2) { rect(x, y - 22, 4, 26, ink); rect(x - 7, y - 15, 18, 4, ink); }
		else { rect(x - 8, y - 14, 18, 18, ink); C2D_DrawCircleSolid(x + 1, y - 14, Z, 9, ink); }
	}
	C2D_DrawLine(370, 240, ink, 368, 150, ink, 7, Z);
	C2D_DrawLine(368, 175, ink, 340, 150, ink, 3, Z);
	C2D_DrawLine(368, 165, ink, 392, 138, ink, 3, Z);
	C2D_DrawLine(352, 162, ink, 344, 140, ink, 2, Z);
	// Mist along the ground.
	for (int i = 0; i < 3; i++) {
		float x = fmodf(t * (12 + i * 5) + i * 160, 600) - 100;
		C2D_DrawEllipseSolid(x, 205 + i * 9, Z, 200, 20, rgba(120, 90, 140, 40));
	}
	sabbath_world(t, 0, 0);
	if (bolt.level > 0) rect(0, 0, TOP_W, SCR_H, rgba(200, 190, 255, (u8)(110 * bolt.level * bolt.level)));
}

// One jack-o'-lantern; the candle inside flickers.
static void draw_pumpkin(float x, float y, float r, float t, int seed)
{
	C2D_DrawEllipseSolid(x - r * 1.2f, y - r, Z, r * 2.4f, r * 2, rgba(230, 100, 10, 255));
	C2D_DrawEllipseSolid(x - r * 0.5f, y - r, Z, r, r * 2, rgba(250, 130, 20, 255));
	rect(x - 2, y - r - 6, 4, 7, rgba(60, 90, 20, 255));
	float fl = 0.7f + 0.3f * sinf(t * 17 + seed) * sinf(t * 7.3f + seed * 2);
	u32 glow = rgba(255, (u8)(200 * fl + 40), 60, 255);
	tri(x - r * 0.65f, y - r * 0.15f, x - r * 0.15f, y - r * 0.15f, x - r * 0.4f, y - r * 0.55f, glow);
	tri(x + r * 0.15f, y - r * 0.15f, x + r * 0.65f, y - r * 0.15f, x + r * 0.4f, y - r * 0.55f, glow);
	for (int i = 0; i < 4; i++) { // jagged grin
		float mx = x - r * 0.7f + i * r * 0.35f;
		tri(mx, y + r * 0.2f, mx + r * 0.35f, y + r * 0.2f, mx + r * 0.175f, y + r * (i % 2 ? 0.6f : 0.45f), glow);
	}
}

// Bottom: the graveyard floor under the storm: mist, glowing pumpkins.
static void draw_sabbath_bottom(float t)
{
	C2D_DrawRectangle(0, 0, Z, BOT_W, SCR_H, rgba(20, 6, 22, 255), rgba(20, 6, 22, 255),
	                  rgba(8, 2, 8, 255), rgba(8, 2, 8, 255));
	for (int i = 0; i < 5; i++) {
		float x = fmodf(t * (10 + i * 4) + i * 120, 520) - 100;
		C2D_DrawEllipseSolid(x, 40 + i * 42, Z, 220, 26, rgba(120, 90, 140, 36));
	}
	draw_pumpkin(26, 212, 14, t, 1);
	draw_pumpkin(294, 210, 16, t, 2);
	draw_pumpkin(300, 40, 11, t, 3);
	sabbath_world(t, BOT_OX, BOT_OY);
	if (bolt.level > 0) rect(0, 0, BOT_W, SCR_H, rgba(200, 190, 255, (u8)(110 * bolt.level * bolt.level)));
}

// ==== Deadhead: bottom screen ================================================

static void draw_bear(float x, float ground, float step, u32 c); // Konami bears, below

// A skull crowned with roses (an original design, not the band's logo).
static void draw_skull(float x, float y, float s, float t)
{
	u32 bone = rgba(245, 236, 215, 255), hole = rgba(30, 12, 40, 255);
	C2D_DrawEllipseSolid(x - 24 * s, y - 26 * s, Z, 48 * s, 44 * s, bone);  // cranium
	rrect(x - 15 * s, y + 8 * s, 30 * s, 16 * s, 5 * s, bone);              // jaw
	C2D_DrawEllipseSolid(x - 16 * s, y - 8 * s, Z, 13 * s, 14 * s, hole);   // eyes
	C2D_DrawEllipseSolid(x + 3 * s, y - 8 * s, Z, 13 * s, 14 * s, hole);
	float glow = 0.5f + 0.5f * sinf(t * 3);                                  // eyes glow to the beat
	C2D_DrawCircleSolid(x - 9.5f * s, y - 1 * s, Z, 2.5f * s, theme_hue(t * 0.1f, 0.8f, 1, (u8)(120 + 135 * glow)));
	C2D_DrawCircleSolid(x + 9.5f * s, y - 1 * s, Z, 2.5f * s, theme_hue(t * 0.1f + 0.5f, 0.8f, 1, (u8)(120 + 135 * glow)));
	tri(x, y + 4 * s, x - 4 * s, y + 11 * s, x + 4 * s, y + 11 * s, hole);  // nose
	for (int i = 0; i < 5; i++) rect(x - 11 * s + i * 5.5f * s, y + 14 * s, 1.2f * s, 9 * s, hole); // teeth
	// Crown of roses with leaves.
	for (int i = 0; i < 7; i++) {
		float a = 3.14159f * (1.1f + i * 0.13f);
		float rx = x + cosf(a) * 24 * s, ry = y - 6 * s + sinf(a) * 24 * s;
		C2D_DrawCircleSolid(rx + 4 * s, ry + 3 * s, Z, 3 * s, rgba(40, 140, 60, 255));
		C2D_DrawCircleSolid(rx, ry, Z, 5 * s, rgba(200, 20, 50, 255));
		C2D_DrawCircleSolid(rx - 1 * s, ry - 1 * s, Z, 2.5f * s, rgba(240, 70, 90, 255));
	}
}

// A terrapin up on its hind legs, shaking a tambourine.
static void draw_terrapin(float x, float ground, float step, bool flip)
{
	float bob = -fabsf(sinf(step)) * 3, g = ground + bob, d = flip ? -1 : 1;
	u32 skin = rgba(120, 190, 90, 255), shell = rgba(60, 120, 50, 255), belly = rgba(230, 210, 120, 255);
	rect(x - 7, g - 7 - fmaxf(0, sinf(step)) * 3, 5, 7, skin);   // legs
	rect(x + 2, g - 7 - fmaxf(0, -sinf(step)) * 3, 5, 7, skin);
	C2D_DrawEllipseSolid(x - 11, g - 30, Z, 22, 26, shell);       // shell
	C2D_DrawEllipseSolid(x - 7, g - 27, Z, 14, 20, belly);        // plastron
	for (int i = 0; i < 3; i++) rect(x - 5, g - 22 + i * 5, 10, 1, rgba(180, 150, 70, 255));
	float arm = sinf(step * 2) * 4;
	rect(x + d * 9 - 1.5f, g - 30 + arm, 3, 10, skin);            // tambourine arm, up
	C2D_DrawCircleSolid(x + d * 10, g - 32 + arm, Z, 5, rgba(220, 180, 90, 255));
	C2D_DrawCircleSolid(x + d * 10, g - 32 + arm, Z, 3, rgba(250, 230, 170, 255));
	rect(x - d * 10 - 1.5f, g - 24 - arm, 3, 8, skin);
	C2D_DrawCircleSolid(x, g - 35, Z, 6, skin);                   // head
	C2D_DrawCircleSolid(x - 2.5f, g - 36, Z, 1.6f, rgba(255, 255, 255, 255));
	C2D_DrawCircleSolid(x + 2.5f, g - 36, Z, 1.6f, rgba(255, 255, 255, 255));
	C2D_DrawCircleSolid(x - 2.5f, g - 36, Z, 0.8f, rgba(10, 10, 10, 255));
	C2D_DrawCircleSolid(x + 2.5f, g - 36, Z, 0.8f, rgba(10, 10, 10, 255));
	rect(x - 2.5f, g - 32, 5, 1, rgba(30, 60, 20, 255));          // grin
}

// A crow in flight, heading right. World coordinates.
static void draw_crow(float x, float y, float flap, float s)
{
	u32 c = rgba(12, 10, 18, 255);
	float up = sinf(flap) * 9 * s;
	tri(x - 4 * s, y, x + 4 * s, y, x - 6 * s, y - 4 * s - up, c);           // far wing
	tri(x - 6 * s, y - 4 * s - up, x + 2 * s, y, x - 14 * s, y - 2 * s - up * 1.3f, c);
	C2D_DrawEllipseSolid(x - 9 * s, y - 3 * s, Z, 18 * s, 6 * s, c);          // body
	C2D_DrawCircleSolid(x + 9 * s, y - 2 * s, Z, 3.5f * s, c);                // head
	tri(x + 12 * s, y - 3 * s, x + 12 * s, y - 0.5f * s, x + 17 * s, y - 1.5f * s, rgba(60, 50, 40, 255)); // beak
	tri(x - 9 * s, y, x - 16 * s, y - 3 * s, x - 15 * s, y + 3 * s, c);      // tail
	tri(x - 2 * s, y, x + 6 * s, y, x - 4 * s, y + 4 * s + up, c);           // near wing
	tri(x - 4 * s, y + 4 * s + up, x + 4 * s, y, x - 12 * s, y + 3 * s + up * 1.3f, c);
}

// A loose flock crossing both screens.
static void deadhead_crows(float t, float ox, float oy)
{
	for (int i = 0; i < 5; i++) {
		float p = fmodf(t * (0.045f + i * 0.01f) + i * 0.29f, 1.0f);
		float x = -40 + p * 500 - ox;
		float y = 50 + i * 85 + sinf(t * 0.9f + i * 1.7f) * 20 - oy;
		draw_crow(x, y, t * 8 + i * 1.3f, i % 2 ? 0.8f : 1.0f);
	}
}

static void draw_deadhead_bottom(float t, bool playing)
{
	draw_tiedye(BOT_W / 2.0f, SCR_H / 2.0f, 210, t);
	rect(0, 0, BOT_W, SCR_H, rgba(20, 10, 36, 90));
	draw_skull(BOT_W / 2.0f, 120 + sinf(t * 1.5f) * 4, 1.4f, t);
	float step = t * (playing ? 7.0f : 2.0f);
	static const float BX[4] = { 22, 66, 254, 298 };   // bears dance up top,
	for (int i = 0; i < 4; i++)                          // clear of the play button
		draw_bear(BX[i], 34, step + i * 1.1f, theme_hue(i / 4.0f + t * 0.05f, 0.7f, 1.0f, 0xFF));
	draw_terrapin(30, 182, step, false);                 // turtles on the sides
	draw_terrapin(290, 182, step + 1.6f, true);
	deadhead_crows(t, BOT_OX, BOT_OY);
}

// Background for the bottom screen when a song effect is on. Text screens
// other than Now Playing get a heavier dim so lists stay readable.
static bool fx_playing;
static void draw_fx_bottom(bool now_screen)
{
	if (fx_mode == FX_LASER) draw_lasers_bottom(fx_phase);
	else if (fx_mode == FX_SABBATH) draw_sabbath_bottom(fx_phase);
	else if (fx_mode == FX_TIEDYE) draw_deadhead_bottom(fx_phase, fx_playing);
	else return;
	rect(0, 0, BOT_W, SCR_H, (C.overlay & 0x00FFFFFF) | (now_screen ? 0x60000000 : 0xB8000000));
}

// ==== Konami bears ===========================================================

static bool bears;

// One bear standing up on its hind legs. step = walk cycle phase.
static void draw_bear(float x, float ground, float step, u32 c)
{
	float bob = -fabsf(sinf(step)) * 3;
	float lift0 = fmaxf(0, sinf(step)) * 4, lift1 = fmaxf(0, -sinf(step)) * 4;
	float g = ground + bob;
	rect(x - 5, g - 9 - lift0, 4, 9, c);                 // legs
	rect(x + 1, g - 9 - lift1, 4, 9, c);
	C2D_DrawCircleSolid(x, g - 14, Z, 7, c);             // belly
	float arm = sinf(step * 2) * 3;                      // arms up, waving
	rect(x - 9, g - 24 + arm, 3, 9, c);
	rect(x + 6, g - 24 - arm, 3, 9, c);
	C2D_DrawCircleSolid(x, g - 25, Z, 5.5f, c);          // head
	C2D_DrawCircleSolid(x - 4, g - 30, Z, 2.2f, c);      // ears
	C2D_DrawCircleSolid(x + 4, g - 30, Z, 2.2f, c);
	C2D_DrawCircleSolid(x + 2, g - 23.5f, Z, 2.2f, rgba(255, 255, 255, 170)); // snout
	C2D_DrawCircleSolid(x - 2, g - 27, Z, 0.9f, rgba(20, 10, 30, 255));      // eye
}

// A parade along the bottom of the top screen. Marches while music plays,
// dances in place while paused.
static void draw_bears(float t, bool playing)
{
	static float march, step;
	march += (playing ? 38.0f : 0.0f) / 60.0f;
	step += (playing ? 7.0f : 2.0f) / 60.0f;
	for (int i = 0; i < 5; i++) {
		float x = fmodf(march + i * 96, 480) - 40;
		draw_bear(x, 204, step + i * 1.1f, theme_hue(i / 5.0f + t * 0.05f, 0.7f, 1.0f, 0xFF));
	}
}

// ==== Spinning vinyl =========================================================

static bool scrubbing;
static float scrub_pos; // seconds, while the circle pad is held
static int scrub_dir;

// Annulus segment from angle a0 to a1 (radians).
static void ring_arc(float cx, float cy, float r0, float r1, float a0, float a1, int segs, u32 c)
{
	for (int i = 0; i < segs; i++) {
		float p = a0 + (a1 - a0) * i / segs, q = a0 + (a1 - a0) * (i + 1) / segs;
		float cp = cosf(p), sp = sinf(p), cq = cosf(q), sq = sinf(q);
		tri(cx + cp * r0, cy + sp * r0, cx + cp * r1, cy + sp * r1, cx + cq * r1, cy + sq * r1, c);
		tri(cx + cp * r0, cy + sp * r0, cx + cq * r1, cy + sq * r1, cx + cq * r0, cy + sq * r0, c);
	}
}

// Writes art_copy rotated by angle into the next vinyl texture, transparent
// outside the inscribed circle. Nearest-neighbour, 16.16 fixed point, and a
// per-row span so only pixels on the record are touched (~2-3 ms per frame
// on the 2DS; skipped entirely while the record is still).
static void rotate_art(float angle)
{
	enum { N = ART_SIZE };
	static u32 tx[N], ty[N]; // Morton offsets split into x and y halves
	static bool init;
	if (!init) {
		for (u32 i = 0; i < N; i++) {
			tx[i] = ((i >> 3) << 6) | (i & 1) | ((i & 2) << 1) | ((i & 4) << 2);
			ty[i] = (((i >> 3) * (N >> 3)) << 6) | ((i & 1) << 1) | ((i & 2) << 2) | ((i & 4) << 3);
		}
		init = true;
	}
	vinyl_buf ^= 1;
	// Pixels outside the circle are never written, so they stay transparent
	// from the one-time clear in ui_init: no 256 KB memset per frame.
	u32 *dst = (u32 *)vinyl_tex[vinyl_buf].data;
	const float h = N / 2.0f, R = h - 1;
	// Destination (dx,dy) from the center samples the source at the point
	// rotated by -angle: u = c*dx + s*dy, v = -s*dx + c*dy.
	float c = cosf(angle), sn = sinf(angle);
	s32 du = (s32)(c * 65536), dv = (s32)(-sn * 65536);
	for (int y = 0; y < N; y++) {
		float dy = y + 0.5f - h;
		float span2 = R * R - dy * dy;
		if (span2 <= 0) continue;
		int half = (int)sqrtf(span2);
		int x0 = (int)h - half, x1 = (int)h + half;
		float dx = x0 + 0.5f - h;
		s32 u = (s32)((c * dx + sn * dy + h) * 65536), v = (s32)((-sn * dx + c * dy + h) * 65536);
		u32 row = ty[y];
		for (int x = x0; x < x1; x++, u += du, v += dv) {
			int su = u >> 16, sv = v >> 16;
			if ((unsigned)su >= N || (unsigned)sv >= N) continue;
			dst[row + tx[x]] = art_copy[sv * N + su];
		}
	}
	C3D_TexFlush(&vinyl_tex[vinyl_buf]);
}

// The album art IS the record: it fills the whole disc and spins at
// 33 1/3 rpm, with grooves, a rim and a spindle drawn over it. Spins up and
// down like a real turntable instead of starting and stopping instantly.
static void draw_vinyl(float cx, float cy, float r, bool playing)
{
	const float TAU = 6.2831853f, RPM33 = TAU * (100.0f / 3 / 60) / 60; // rad per frame
	static float angle, speed, drawn_angle = -1;
	static u64 last_ms;
	// Advance by real elapsed time, not per frame: when the CPU is busy and
	// a frame is skipped, the record still turns the right amount instead
	// of visibly slowing down. dt is in 60 Hz frames.
	u64 now = osGetTime();
	float dt = last_ms ? (now - last_ms) / (1000.0f / 60) : 1;
	last_ms = now;
	if (dt > 6) dt = 6; // after a pause / screen switch
	float target = scrubbing ? scrub_dir * RPM33 * 8 : playing ? RPM33 : 0;
	float ease = 1 - powf(1 - (scrubbing ? 0.2f : 0.04f), dt);
	speed += (target - speed) * ease;
	if (fabsf(speed) < 0.0005f && !target) speed = 0;
	angle = fmodf(angle + speed * dt, TAU);

	const u32 VINYL = rgba(14, 14, 16, 255);
	C2D_DrawCircleSolid(cx, cy, Z, r + 1, VINYL);
	if (art_valid && art_copy) {
		if (vinyl_dirty || angle != drawn_angle) {
			rotate_art(angle);
			drawn_angle = angle;
			vinyl_dirty = false;
		}
		C2D_Image img = { &vinyl_tex[vinyl_buf], &vinyl_sub };
		C2D_DrawImageAt(img, cx - r, cy - r, Z, NULL, 2 * r / ART_SIZE, 2 * r / ART_SIZE);
	} else {
		// No art: a plain black record with a colored label.
		C2D_DrawCircleSolid(cx, cy, Z, 30, C.accent);
		C2D_DrawCircleSolid(cx + cosf(angle) * 20, cy + sinf(angle) * 20, Z, 3, C.on_accent);
	}
	// Grooves: faint dark rings over the art, so it reads as pressed vinyl.
	for (float g = 16; g < r - 3; g += 4) ring_arc(cx, cy, g, g + 1, 0, TAU, 40, rgba(0, 0, 0, 46));
	ring_arc(cx, cy, r - 3, r + 1, 0, TAU, 48, VINYL); // rim (also hides the jaggy edge)
	// Light reflections stay put while the record turns under them.
	ring_arc(cx, cy, 14, r - 3, -2.4f, -1.95f, 6, rgba(255, 255, 255, 34));
	ring_arc(cx, cy, 14, r - 3, 0.75f, 1.2f, 6, rgba(255, 255, 255, 34));
	C2D_DrawCircleSolid(cx, cy, Z, 9, VINYL);                       // center label hole
	C2D_DrawCircleSolid(cx, cy, Z, 2.5f, rgba(200, 200, 200, 255)); // spindle
}

static void fmt_time(int s, char *out, size_t cap)
{
	if (s < 0) s = 0;
	if (s >= 3600) snprintf(out, cap, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
	else snprintf(out, cap, "%d:%02d", s / 60, s % 60);
}

static const char *spinner(void)
{
	static const char *f[] = { ".  ", ".. ", "...", " ..", "  .", "   " };
	return f[(frame_no / 10) % 6];
}

static bool touch_over(const input_t *in, int x, int y, int w, int h)
{
	return in->touching && input_holding_in(in, x, y, w, h) && in->tx >= x && in->tx < x + w && in->ty >= y && in->ty < y + h;
}

// ==== album art =============================================================

static void sync_art(ctrl_state_t *st)
{
	if (st->art_gen == art_gen_seen) return;
	art_gen_seen = st->art_gen;
	art_valid = st->art_pixels != NULL;
	if (!art_valid) return;

	// GPU textures are stored in 8x8 tiles with Morton (Z-order) inside each tile.
	u32 *dst = (u32 *)art_tex.data;
	const u32 *src = st->art_pixels;
	for (u32 y = 0; y < ART_SIZE; y++) {
		for (u32 x = 0; x < ART_SIZE; x++) {
			u32 tile = ((y >> 3) * (ART_SIZE >> 3) + (x >> 3)) << 6;
			u32 m = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
			dst[tile + m] = src[y * ART_SIZE + x];
		}
	}
	C3D_TexFlush(&art_tex);
	if (!art_copy) art_copy = malloc(ART_SIZE * ART_SIZE * 4);
	if (art_copy) memcpy(art_copy, src, ART_SIZE * ART_SIZE * 4);
	vinyl_dirty = true;
}

// ==== top screen: Now Playing ================================================

static void draw_toast(ctrl_state_t *st)
{
	if (osGetTime() >= st->toast_until || !st->toast[0]) return;
	float w = measure(st->toast, 12) + 24;
	if (w > TOP_W - 24) w = TOP_W - 24;
	float x = (TOP_W - w) / 2, y = 186;
	rrect(x, y, w, 22, 11, st->toast_is_error ? C.err : C.raised);
	text(st->toast, TOP_W / 2.0f, y + 4, 12, st->toast_is_error ? C.bg : C.text, AL_CENTER, w - 16);
}

static void draw_center_message(const char *title, const char *line1, const char *line2)
{
	text(title, TOP_W / 2.0f, 84, 18, C.text, AL_CENTER, TOP_W - 40);
	if (line1) text(line1, TOP_W / 2.0f, 116, 12, C.muted, AL_CENTER, TOP_W - 40);
	if (line2) text(line2, TOP_W / 2.0f, 134, 12, C.muted, AL_CENTER, TOP_W - 40);
}

static void draw_top(ctrl_state_t *st)
{
	C2D_TargetClear(top, C.bg);
	C2D_SceneBegin(top);
	bool playing = st->ts == TS_PLAYING;
	fx_playing = playing;
	if (fx_mode) {
		// Swirl faster while music plays, drift when paused.
		fx_phase += (playing ? 1.0f : 0.3f) / 60.0f;
		if (fx_mode == FX_LASER) {
			draw_lasers(fx_phase);
			rect(0, 0, TOP_W, SCR_H, (C.overlay & 0x00FFFFFF) | 0x78000000); // lighter dim: let it glow
		} else if (fx_mode == FX_SABBATH) {
			sabbath_tick(playing);
			draw_sabbath_top(fx_phase);
			rect(0, 0, TOP_W, SCR_H, (C.overlay & 0x00FFFFFF) | 0x70000000);
		} else {
			draw_tiedye(TOP_W / 2.0f, SCR_H / 2.0f, 240, fx_phase);
			deadhead_crows(fx_phase, 0, 0);
			rect(0, 0, TOP_W, SCR_H, C.overlay & 0x9FFFFFFF); // dim to ~62% so text stays readable
		}
	}

	const sonos_speaker_t *room = ctrl_active_speaker(st);

	// status bar
	rect(0, 0, TOP_W, 20, C.surface);
	bool online = st->net == NET_UP && room && st->reachable;
	C2D_DrawCircleSolid(11, 10, Z, 3, online ? C.ok : C.err);
	char label[96];
	if (room && room->group_size > 1) snprintf(label, sizeof label, "%s + %d", room->name, room->group_size - 1);
	else snprintf(label, sizeof label, "%s", room ? room->name : "No room selected");
	text(label, 20, 3, 12, C.muted, AL_LEFT, 250);

	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	char clock[16];
	if (g_settings.clock_24h) strftime(clock, sizeof clock, "%H:%M", tm);
	else strftime(clock, sizeof clock, "%I:%M %p", tm);
	float cw = text(clock, TOP_W - 8, 3, 12, C.muted, AL_RIGHT, 0);
	wifi_bars(TOP_W - 8 - cw - 22, 5, osGetWifiStrength(), st->net == NET_UP);

	if (st->net != NET_UP) {
		draw_center_message("Not connected to Wi-Fi", "Turn on wireless and connect in System Settings.",
		                    "Sonnis will reconnect automatically.");
	} else if (st->discovering && st->speaker_count == 0) {
		char t[64];
		snprintf(t, sizeof t, "%s%s", st->scanning ? "Scanning network" : "Searching for Sonos", spinner());
		draw_center_message(t, st->scanning ? "Discovery is blocked here; checking each address"
		                                    : "Looking for speakers on your network", NULL);
	} else if (st->speaker_count == 0) {
		draw_center_message("No speakers found", "Check the 2DS is on the same Wi-Fi as your Sonos.",
		                    "START > Add speaker IP, or Refresh discovery.");
	} else if (!room) {
		draw_center_message("Pick a room", "Tap Rooms on the touch screen", NULL);
	} else if (!st->have_status) {
		char t[96];
		snprintf(t, sizeof t, "Connecting to %s%s", room->name, spinner());
		draw_center_message(t, room->ip, NULL);
	} else {
		// album art
		if (g_settings.vinyl) draw_vinyl(89, 109, 75, playing);
		else if (art_valid) C2D_DrawImageAt(art_img, 14, 34, Z, NULL, 150.0f / ART_SIZE, 150.0f / ART_SIZE);
		else {
			rrect(14, 34, 150, 150, 8, C.raised);
			icon_note(89, 109, 48, C.muted);
		}

		const track_info_t *t = &st->track;
		float tx = 178, tw = TOP_W - 178 - 14;
		const char *state_lbl = st->ts == TS_PLAYING ? "NOW PLAYING" : st->ts == TS_PAUSED ? "PAUSED"
		                       : st->ts == TS_TRANSITIONING ? "LOADING" : "STOPPED";
		text(state_lbl, tx, 38, 11, C.accent, AL_LEFT, tw);
		float y = 54;
		if (t->title[0]) {
			int lines = text_wrap2(t->title, tx, y, 18, 21, C.text, tw);
			y += lines * 21 + 4;
			if (t->artist[0]) { text(t->artist, tx, y, 13, C.text, AL_LEFT, tw); y += 17; }
			if (t->album[0]) text(t->album, tx, y, 12, C.muted, AL_LEFT, tw);
		} else {
			text("Nothing playing", tx, y, 18, C.text, AL_LEFT, tw);
			text("Pick something in Browse", tx, y + 25, 12, C.muted, AL_LEFT, tw);
		}

		// play-mode chips
		float cx = tx;
		const char *rep = st->repeat == REPEAT_ALL ? "REPEAT ALL" : st->repeat == REPEAT_ONE ? "REPEAT ONE" : "REPEAT OFF";
		float w1 = measure("SHUFFLE", 11) + 12;
		rrect(cx, 165, w1, 18, 4, C.raised);
		text("SHUFFLE", cx + 6, 168, 11, st->shuffle ? C.accent : C.muted, AL_LEFT, 0);
		cx += w1 + 6;
		float w2 = measure(rep, 11) + 12;
		rrect(cx, 165, w2, 18, 4, C.raised);
		text(rep, cx + 6, 168, 11, st->repeat != REPEAT_OFF ? C.accent : C.muted, AL_LEFT, 0);

		// progress
		int el = scrubbing ? (int)scrub_pos : ctrl_estimated_elapsed(st);
		float pw = TOP_W - 28;
		rrect(14, 206, pw, 4, 2, C.line);
		if (t->duration_s > 0) {
			float f = (float)el / (float)t->duration_s;
			if (f > 1) f = 1;
			if (f > 0 && fx_mode) rainbow_bar(14, 206, pw * f, 4, fx_time());
			else if (f > 0) rrect(14, 206, pw * f, 4, 2, C.accent);
		}
		char a[16], b[16], v[24];
		fmt_time(el, a, sizeof a);
		if (t->duration_s > 0) fmt_time(t->duration_s, b, sizeof b); else str_copy(b, "LIVE", sizeof b);
		if (st->muted) str_copy(v, "MUTED", sizeof v); else snprintf(v, sizeof v, "VOL %d", st->volume);
		text(a, 14, 213, 12, C.muted, AL_LEFT, 0);
		text(v, TOP_W / 2.0f, 213, 12, st->muted ? C.accent : C.muted, AL_CENTER, 0);
		text(b, TOP_W - 14, 213, 12, C.muted, AL_RIGHT, 0);
	}
	if (bears) draw_bears(fx_time(), playing);
	draw_toast(st);
}

// ==== shared list widget (bottom screen) ====================================

#define ROW_H 36
#define HEADER_H 32

typedef struct {
	char title[128];
	char subtitle[96];
	char right[48];
	bool selected_mark;   // accent bar (e.g. active room / playing track)
	bool dim;
	bool has_dot;
	u32 dot;
	bool pin;
	bool play_button;     // container that can be played directly
} row_t;

typedef void (*row_fn)(int i, row_t *r, void *ctx);

static void clamp_scroll(float *scroll, int count, float area_h)
{
	float max = count * ROW_H - area_h;
	if (max < 0) max = 0;
	if (*scroll < 0) *scroll = 0;
	if (*scroll > max) *scroll = max;
}

// D-pad / drag navigation. Returns the index of a tapped row, or -1.
static int list_input(const input_t *in, int *cursor, float *scroll, int count, float y0, float area_h)
{
	if (count <= 0) return -1;
	int old = *cursor;
	if (in->repeat & KEY_DOWN) (*cursor)++;
	if (in->repeat & KEY_UP) (*cursor)--;
	if (in->repeat & KEY_RIGHT) *cursor += (int)(area_h / ROW_H);
	if (in->repeat & KEY_LEFT) *cursor -= (int)(area_h / ROW_H);
	if (*cursor >= count) *cursor = (old == count - 1 && (in->repeat & KEY_DOWN)) ? 0 : count - 1; // wrap
	if (*cursor < 0) *cursor = (old == 0 && (in->repeat & KEY_UP)) ? count - 1 : 0;

	if (*cursor != old) { // keep cursor visible
		float top_y = *cursor * ROW_H, bot_y = top_y + ROW_H;
		if (top_y < *scroll) *scroll = top_y;
		if (bot_y > *scroll + area_h) *scroll = bot_y - area_h;
	}
	if (input_holding_in(in, 0, (int)y0, BOT_W, (int)area_h) && in->dragged) *scroll -= in->dy_frame;
	clamp_scroll(scroll, count, area_h);

	if (input_tapped_in(in, 0, (int)y0, BOT_W, (int)area_h)) {
		int i = (int)((in->sy - y0 + *scroll) / ROW_H);
		if (i >= 0 && i < count) { *cursor = i; return i; }
	}
	return -1;
}

static void draw_list(int count, int cursor, float scroll, float y0, float area_h, row_fn fn, void *ctx)
{
	int first = (int)(scroll / ROW_H);
	for (int i = first; i < count && i * ROW_H - scroll < area_h; i++) {
		float y = y0 + i * ROW_H - scroll;
		row_t r;
		memset(&r, 0, sizeof r);
		fn(i, &r, ctx);
		bool cur = i == cursor;
		if (cur) rect(0, y, BOT_W, ROW_H, C.raised);
		if (cur || r.selected_mark) rect(0, y, 3, ROW_H, r.selected_mark ? C.accent : C.muted);
		rect(0, y + ROW_H - 1, BOT_W, 1, C.line);

		float x = 12, right_edge = BOT_W - 10;
		if (r.has_dot) { C2D_DrawCircleSolid(x + 4, y + ROW_H / 2.0f, Z, 4, r.dot); x += 14; }
		if (r.play_button) {
			rrect(BOT_W - 40, y + 6, 32, 24, 6, cur ? C.accent : C.surface);
			icon_play(BOT_W - 24, y + 18, 10, cur ? C.on_accent : C.text);
			right_edge = BOT_W - 46;
		}
		if (r.right[0]) {
			float rw = text(r.right, right_edge, y + 11, 12, C.muted, AL_RIGHT, 120);
			right_edge -= rw + 8;
		}
		if (r.pin) { icon_pin(right_edge - 4, y + ROW_H / 2.0f, 8, C.accent); right_edge -= 14; }
		u32 tc = r.dim ? C.muted : C.text;
		if (r.subtitle[0]) {
			text(r.title, x, y + 3, 14, tc, AL_LEFT, right_edge - x);
			text(r.subtitle, x, y + 20, 12, C.muted, AL_LEFT, right_edge - x);
		} else {
			text(r.title, x, y + 10, 15, tc, AL_LEFT, right_edge - x);
		}
	}
	// scrollbar
	float content = count * ROW_H;
	if (content > area_h) {
		float h = area_h * area_h / content, y = y0 + (area_h - h) * (scroll / (content - area_h));
		rect(BOT_W - 3, y, 2, h, C.muted);
	}
}

static void draw_header(const char *title, bool back, const char *action)
{
	rect(0, 0, BOT_W, HEADER_H, C.surface);
	float x = 8;
	if (back) {
		rrect(8, 4, 30, 24, 6, C.raised);
		tri(27, 9, 27, 23, 18, 16, C.text);
		x = 46;
	}
	if (action) {
		rrect(BOT_W - 8 - 76, 4, 76, 24, 6, C.raised);
		text(action, BOT_W - 8 - 38, 8, 12, C.text, AL_CENTER, 70);
	}
	text(title, x, 7, 15, C.text, AL_LEFT, (action ? BOT_W - 92 : BOT_W - 8) - x);
}

static bool header_back_tapped(const input_t *in) { return input_pressed_in(in, 0, 0, 44, HEADER_H); }
static bool header_action_tapped(const input_t *in) { return input_pressed_in(in, BOT_W - 88, 0, 88, HEADER_H); }

static void draw_footer_hint(const char *left, const char *right)
{
	rect(0, 212, BOT_W, 28, C.surface);
	text(left, 8, 218, 12, C.muted, AL_LEFT, 230);
	if (right) text(right, BOT_W - 8, 218, 12, C.muted, AL_RIGHT, 80);
}

// ==== bottom: Now Playing controls ==========================================

#define NAV_Y 184
#define NAV_H 48

static void nav_rect(int i, float *x, float *w) { *w = (BOT_W - 16 - 18) / 4.0f; *x = 8 + i * (*w + 6); }

static void open_browse_at(const char *id, const char *title)
{
	bdepth = 0;
	if (id) {
		bdepth = 1;
		memset(&bstack[1], 0, sizeof bstack[1]);
		str_copy(bstack[1].id, id, sizeof bstack[1].id);
		str_copy(bstack[1].title, title, sizeof bstack[1].title);
		ctrl_send(CMD_BROWSE, 0, 0, id);
	}
	screen = SCR_BROWSE;
}

static void now_input(const input_t *in)
{
	if (in->down & KEY_A) ctrl_send(CMD_PLAY_PAUSE, 0, 0, NULL);
	if (in->down & KEY_X) ctrl_send(CMD_NEXT, 0, 0, NULL);
	if (in->down & KEY_Y) ctrl_send(CMD_PREV, 0, 0, NULL);
	if (in->down & KEY_DLEFT) ctrl_send(CMD_SHUFFLE_TOGGLE, 0, 0, NULL);
	if (in->down & KEY_DRIGHT) ctrl_send(CMD_REPEAT_CYCLE, 0, 0, NULL);
	if (in->down & KEY_DUP) screen = SCR_ROOMS;
	if (in->down & KEY_DDOWN) open_browse_at(NULL, NULL);

	// Vinyl: hold the circle pad left/right to spin the record and scrub
	// through the song; letting go seeks there.
	if (g_settings.vinyl) {
		int dir = (in->held & KEY_CPAD_RIGHT) ? 1 : (in->held & KEY_CPAD_LEFT) ? -1 : 0;
		ctrl_lock();
		ctrl_state_t *st = ctrl_state();
		int dur = st->track.duration_s;
		if (dir && dur > 0) {
			if (!scrubbing) scrub_pos = (float)ctrl_estimated_elapsed(st);
			scrubbing = true;
			scrub_dir = dir;
			scrub_pos += dir * 25.0f / 60.0f; // 25 seconds of song per second held
			if (scrub_pos < 0) scrub_pos = 0;
			if (scrub_pos > dur - 1) scrub_pos = (float)(dur - 1);
		} else if (scrubbing) {
			scrubbing = false;
			st->track.elapsed_s = (int)scrub_pos; // optimistic; the next poll corrects it
			st->track_tick = osGetTime();
			ctrl_send(CMD_SEEK, (int)scrub_pos, 0, NULL);
		}
		ctrl_unlock();
	}

	if (input_pressed_in(in, 78 - 28, 59 - 28, 56, 56)) ctrl_send(CMD_PREV, 0, 0, NULL);
	if (input_pressed_in(in, 160 - 36, 59 - 36, 72, 72)) ctrl_send(CMD_PLAY_PAUSE, 0, 0, NULL);
	if (input_pressed_in(in, 242 - 28, 59 - 28, 56, 56)) ctrl_send(CMD_NEXT, 0, 0, NULL);
	if (input_pressed_in(in, 12, 106, 44, 44)) ctrl_send(CMD_MUTE_TOGGLE, 0, 0, NULL);
	if (input_pressed_in(in, 60, 150, 96, 30)) ctrl_send(CMD_SHUFFLE_TOGGLE, 0, 0, NULL);
	if (input_pressed_in(in, 164, 150, 96, 30)) ctrl_send(CMD_REPEAT_CYCLE, 0, 0, NULL);

	// Volume slider: drag anywhere along the track (generous hit area).
	if (input_holding_in(in, 56, 106, 238, 44)) {
		int v = (int)((in->tx - 62) * 100.0f / 218.0f + 0.5f);
		if (v < 0) v = 0;
		if (v > 100) v = 100;
		ctrl_lock();
		int cur = ctrl_state()->volume;
		ctrl_unlock();
		if (v != cur) ctrl_set_volume(v);
	}

	for (int i = 0; i < 4; i++) {
		float x, w;
		nav_rect(i, &x, &w);
		if (!input_pressed_in(in, (int)x, NAV_Y, (int)w, NAV_H)) continue;
		if (i == 0) screen = SCR_ROOMS;
		if (i == 1) open_browse_at(NULL, NULL);
		if (i == 2) open_browse_at(LIB_ID_FAVORITES, "Sonos Favorites");
		if (i == 3) screen = SCR_SETTINGS;
	}
}

static void draw_now(const input_t *in, ctrl_state_t *st)
{
	bool playing = st->ts == TS_PLAYING || st->ts == TS_TRANSITIONING;

	C2D_DrawCircleSolid(78, 59, Z, 26, touch_over(in, 50, 31, 56, 56) ? C.raised : C.surface);
	icon_prev(78, 59, 20, C.text);
	if (fx_mode) rainbow_halo(160, 59, 40, fx_time());
	C2D_DrawCircleSolid(160, 59, Z, 34, C.accent);
	if (playing) icon_pause(160, 59, 24, C.on_accent); else icon_play(162, 59, 26, C.on_accent);
	C2D_DrawCircleSolid(242, 59, Z, 26, touch_over(in, 214, 31, 56, 56) ? C.raised : C.surface);
	icon_next(242, 59, 20, C.text);

	// volume
	rrect(16, 110, 36, 36, 8, st->muted ? C.accent : C.surface);
	icon_speaker(34, 128, 18, st->muted, st->muted ? C.on_accent : C.text);
	rrect(62, 124, 218, 8, 4, C.line);
	float vf = st->volume / 100.0f;
	if (vf > 0 && fx_mode && !st->muted) rainbow_bar(62, 124, 218 * vf, 8, fx_time());
	else if (vf > 0) rrect(62, 124, 218 * vf, 8, 4, st->muted ? C.muted : C.text);
	C2D_DrawCircleSolid(62 + 218 * vf, 128, Z, 7, st->muted ? C.muted : C.text);
	char v[8];
	snprintf(v, sizeof v, "%d", st->volume);
	text(v, 310, 120, 13, C.muted, AL_RIGHT, 0);

	// shuffle / repeat pills
	rrect(60, 152, 96, 26, 13, st->shuffle ? C.accent : C.surface);
	text("Shuffle", 108, 156, 13, st->shuffle ? C.on_accent : C.text, AL_CENTER, 90);
	const char *rep = st->repeat == REPEAT_ALL ? "Repeat all" : st->repeat == REPEAT_ONE ? "Repeat one" : "Repeat";
	rrect(164, 152, 96, 26, 13, st->repeat != REPEAT_OFF ? C.accent : C.surface);
	text(rep, 212, 156, 13, st->repeat != REPEAT_OFF ? C.on_accent : C.text, AL_CENTER, 90);

	static const char *labels[4] = { "Rooms", "Browse", "Favorites", "Settings" };
	static const char *hints[4] = { G_UP, G_DOWN, "", "START" };
	for (int i = 0; i < 4; i++) {
		float x, w;
		nav_rect(i, &x, &w);
		rrect(x, NAV_Y, w, NAV_H, 10, touch_over(in, (int)x, NAV_Y, (int)w, NAV_H) ? C.raised : C.surface);
		text(labels[i], x + w / 2, NAV_Y + 8, 14, C.text, AL_CENTER, w - 6);
		text(hints[i], x + w / 2, NAV_Y + 27, 11, C.muted, AL_CENTER, w - 6);
	}
}

// ==== bottom: Browse =========================================================

typedef struct { ctrl_state_t *st; int playing_idx; } browse_ctx_t;

static void root_row(int i, row_t *r, void *ctx)
{
	str_copy(r->title, ROOT[i].title, sizeof r->title);
	str_copy(r->subtitle, ROOT[i].subtitle, sizeof r->subtitle);
	r->dim = !strcmp(ROOT[i].id, LIB_ID_SERVICES);
}

static void browse_row(int i, row_t *r, void *vctx)
{
	browse_ctx_t *ctx = vctx;
	const lib_item_t *it = &ctx->st->browse.items[i];
	str_copy(r->title, it->title[0] ? it->title : "(untitled)", sizeof r->title);
	str_copy(r->subtitle, it->subtitle, sizeof r->subtitle);
	r->dim = it->info_only;
	r->play_button = it->container && it->playable;
	r->selected_mark = i == ctx->playing_idx;
	if (it->container && !it->playable) str_copy(r->right, ">", sizeof r->right);
}

static bool in_queue(void) { return bdepth > 0 && !strcmp(bstack[bdepth].id, LIB_ID_QUEUE); }

static void browse_activate(int idx, bool play_container)
{
	bframe_t *f = &bstack[bdepth];
	if (bdepth == 0) {
		if (idx < 0 || idx >= ROOT_COUNT || bdepth + 1 >= MAX_DEPTH) return;
		bdepth++;
		memset(&bstack[bdepth], 0, sizeof bstack[bdepth]);
		str_copy(bstack[bdepth].id, ROOT[idx].id, sizeof bstack[bdepth].id);
		str_copy(bstack[bdepth].title, ROOT[idx].title, sizeof bstack[bdepth].title);
		ctrl_send(CMD_BROWSE, 0, 0, ROOT[idx].id);
		return;
	}

	// Copy what we need under the lock, then act without it (ctrl_send locks).
	ctrl_lock();
	ctrl_state_t *st = ctrl_state();
	bool ok = idx >= 0 && idx < st->browse.count && !st->browse_loading;
	lib_item_t it = { 0 };
	int gen = st->browse_gen;
	if (ok) { it = st->browse.items[idx]; it.meta = NULL; } // shallow: strings only
	ctrl_unlock();
	if (!ok || it.info_only) return;

	if (it.container && !play_container) {
		if (bdepth + 1 >= MAX_DEPTH) return;
		f->cursor = idx;
		bdepth++;
		memset(&bstack[bdepth], 0, sizeof bstack[bdepth]);
		str_copy(bstack[bdepth].id, it.id, sizeof bstack[bdepth].id);
		str_copy(bstack[bdepth].title, it.title, sizeof bstack[bdepth].title);
		ctrl_send(CMD_BROWSE, 0, 0, it.id);
	} else if (it.playable) {
		ctrl_send(CMD_PLAY_ITEM, idx, gen, NULL);
		if (!in_queue()) screen = SCR_NOW;
	}
}

static void browse_back(void)
{
	if (bdepth == 0) { screen = SCR_NOW; return; }
	bdepth--;
	if (bdepth > 0) ctrl_send(CMD_BROWSE, 0, 0, bstack[bdepth].id);
}

static void browse_input(const input_t *in)
{
	bframe_t *f = &bstack[bdepth];
	if ((in->down & KEY_B) || header_back_tapped(in)) { browse_back(); return; }

	if (bdepth == 0) {
		int tapped = list_input(in, &f->cursor, &f->scroll, ROOT_COUNT, HEADER_H, 180);
		if (tapped >= 0 || (in->down & KEY_A)) browse_activate(f->cursor, false);
		if (in->down & KEY_X) ctrl_send(CMD_NEXT, 0, 0, NULL);
		if (in->down & KEY_Y) ctrl_send(CMD_PREV, 0, 0, NULL);
		return;
	}

	ctrl_lock();
	ctrl_state_t *st = ctrl_state();
	int count = st->browse.count;
	bool loading = st->browse_loading;
	bool want_more = !loading && count < st->browse.total && f->cursor >= count - 8;
	if (want_more) st->browse_loading = true; // claim it so we only ask once
	bool cur_is_container = f->cursor < count && st->browse.items[f->cursor].container;
	ctrl_unlock();
	if (want_more) ctrl_send(CMD_BROWSE, count, 0, f->id);

	int tapped = list_input(in, &f->cursor, &f->scroll, count, HEADER_H, 180);
	if (tapped >= 0) {
		// Tapping the row's play button plays the container instead of opening it.
		bool on_play_btn = in->sx >= BOT_W - 44;
		browse_activate(tapped, on_play_btn);
	}
	if (in->down & KEY_A) browse_activate(f->cursor, false);

	if (in_queue()) {
		if ((in->down & KEY_X) && count > 0) ctrl_send(CMD_QUEUE_REMOVE, f->cursor, 0, NULL);
		if ((in->down & KEY_Y) || header_action_tapped(in)) ctrl_send(CMD_QUEUE_CLEAR, 0, 0, NULL);
	} else {
		if ((in->down & KEY_X) && cur_is_container) browse_activate(f->cursor, true);
		else if (in->down & KEY_X) ctrl_send(CMD_NEXT, 0, 0, NULL);
		if (in->down & KEY_Y) ctrl_send(CMD_PREV, 0, 0, NULL);
		if (header_action_tapped(in)) open_browse_at(NULL, NULL);
	}
}

static void draw_browse(ctrl_state_t *st)
{
	bframe_t *f = &bstack[bdepth];
	if (bdepth == 0) {
		draw_list(ROOT_COUNT, f->cursor, f->scroll, HEADER_H, 180, root_row, NULL);
		draw_header("Browse", true, NULL);
		draw_footer_hint(G_A " open  " G_B " back  " G_UP G_DOWN " move", NULL);
		return;
	}

	int count = st->browse.count;
	if (!st->browse_loading && f->cursor >= count) f->cursor = count > 0 ? count - 1 : 0;
	browse_ctx_t ctx = { st, -1 };
	if (in_queue() && st->have_status) ctx.playing_idx = st->track.track_num - 1;
	draw_list(count, f->cursor, f->scroll, HEADER_H, 180, browse_row, &ctx);

	if (count == 0) {
		char msg[64];
		if (st->browse_loading) snprintf(msg, sizeof msg, "Loading%s", spinner());
		else if (st->browse_err) str_copy(msg, "Couldn't load this list", sizeof msg);
		else str_copy(msg, "Nothing here yet", sizeof msg);
		text(msg, BOT_W / 2.0f, 110, 13, C.muted, AL_CENTER, BOT_W - 20);
	}

	draw_header(f->title, true, in_queue() ? "Clear" : "Home");
	char pos[32] = "";
	if (st->browse.total > 0) snprintf(pos, sizeof pos, "%d / %d", f->cursor + 1, st->browse.total);
	if (in_queue()) draw_footer_hint(G_A " play  " G_X " remove  " G_Y " clear", pos);
	else draw_footer_hint(G_A " open/play  " G_X " play all  " G_B " back", pos);
}

// ==== bottom: Rooms ============================================================

typedef struct { ctrl_state_t *st; int order[MAX_SPEAKERS]; int n; } rooms_ctx_t;

static void rooms_build(ctrl_state_t *st, rooms_ctx_t *rc)
{
	rc->st = st;
	rc->n = 0;
	for (int i = 0; i < st->speaker_count; i++)
		if (!st->speakers[i].invisible) rc->order[rc->n++] = i;
	// pinned first, then alphabetical (insertion sort: n is tiny)
	for (int a = 1; a < rc->n; a++) {
		int v = rc->order[a], b = a - 1;
		const sonos_speaker_t *sv = &st->speakers[v];
		bool pv = settings_is_preferred(&g_settings, sv->uuid);
		while (b >= 0) {
			const sonos_speaker_t *sb = &st->speakers[rc->order[b]];
			bool pb = settings_is_preferred(&g_settings, sb->uuid);
			bool after = (pb && !pv) || (pb == pv && strcasecmp(sb->name, sv->name) <= 0);
			if (after) break;
			rc->order[b + 1] = rc->order[b];
			b--;
		}
		rc->order[b + 1] = v;
	}
}

static void rooms_row(int i, row_t *r, void *vctx)
{
	rooms_ctx_t *rc = vctx;
	ctrl_state_t *st = rc->st;
	const sonos_speaker_t *s = &st->speakers[rc->order[i]];
	str_copy(r->title, s->name, sizeof r->title);
	r->has_dot = true;
	r->dot = s->reachable ? C.ok : C.err;
	r->selected_mark = !strcmp(s->uuid, st->active_uuid);
	r->pin = settings_is_preferred(&g_settings, s->uuid);
	if (!s->reachable) str_copy(r->subtitle, "Unreachable", sizeof r->subtitle);
	else if (s->group_size > 1 && s->is_coordinator)
		snprintf(r->subtitle, sizeof r->subtitle, "Group of %d - leader - %s", s->group_size, s->ip);
	else if (s->group_size > 1) {
		int c = sonos_find_coordinator(st->speakers, st->speaker_count, rc->order[i]);
		snprintf(r->subtitle, sizeof r->subtitle, "Grouped with %s", st->speakers[c].name);
	} else snprintf(r->subtitle, sizeof r->subtitle, "Standalone - %s", s->ip);
}

#define ROOMS_AREA 152

static void rooms_input(const input_t *in)
{
	if ((in->down & KEY_B) || header_back_tapped(in)) { screen = SCR_NOW; return; }
	if (header_action_tapped(in)) ctrl_send(CMD_DISCOVER, 0, 0, NULL);

	ctrl_lock();
	rooms_ctx_t rc;
	rooms_build(ctrl_state(), &rc);
	char uuid[48] = "";
	int tapped = list_input(in, &rooms_cursor, &rooms_scroll, rc.n, HEADER_H, ROOMS_AREA);
	if (rooms_cursor < rc.n) str_copy(uuid, rc.st->speakers[rc.order[rooms_cursor]].uuid, sizeof uuid);
	ctrl_unlock();
	if (!uuid[0]) return;

	float bw = (BOT_W - 16 - 12) / 3.0f;
	bool join = (in->down & KEY_X) || input_pressed_in(in, 8, 192, (int)bw, 40);
	bool leave = (in->down & KEY_Y) || input_pressed_in(in, (int)(8 + bw + 6), 192, (int)bw, 40);
	bool pin = input_pressed_in(in, (int)(8 + 2 * (bw + 6)), 192, (int)bw, 40);

	if (tapped >= 0 || (in->down & KEY_A)) {
		ctrl_send(CMD_SELECT_ROOM, 0, 0, uuid);
		str_copy(g_settings.last_room_uuid, uuid, sizeof g_settings.last_room_uuid);
		settings_save(&g_settings);
		screen = SCR_NOW;
	}
	if (join) ctrl_send(CMD_JOIN, 0, 0, uuid);
	if (leave) ctrl_send(CMD_LEAVE, 0, 0, uuid);
	if (pin) { settings_toggle_preferred(&g_settings, uuid); settings_save(&g_settings); }
}

static void draw_rooms(const input_t *in, ctrl_state_t *st)
{
	rooms_ctx_t rc;
	rooms_build(st, &rc);
	if (rooms_cursor >= rc.n) rooms_cursor = rc.n > 0 ? rc.n - 1 : 0;
	draw_list(rc.n, rooms_cursor, rooms_scroll, HEADER_H, ROOMS_AREA, rooms_row, &rc);
	if (rc.n == 0) {
		char msg[64];
		if (st->discovering) snprintf(msg, sizeof msg, "Searching%s", spinner());
		else str_copy(msg, "No rooms found", sizeof msg);
		text(msg, BOT_W / 2.0f, 96, 13, C.muted, AL_CENTER, BOT_W - 20);
	}
	draw_header("Rooms", true, st->discovering ? "Searching" : "Refresh");

	rect(0, HEADER_H + ROOMS_AREA, BOT_W, SCR_H - HEADER_H - ROOMS_AREA, C.bg);
	float bw = (BOT_W - 16 - 12) / 3.0f;
	static const char *lbl[3] = { G_X " Join mine", G_Y " Leave", "Pin room" };
	for (int i = 0; i < 3; i++) {
		float x = 8 + i * (bw + 6);
		rrect(x, 192, bw, 40, 10, touch_over(in, (int)x, 192, (int)bw, 40) ? C.raised : C.surface);
		text(lbl[i], x + bw / 2, 203, 12, C.text, AL_CENTER, bw - 6);
	}
}

// ==== bottom: Settings ==========================================================

enum {
	SET_THEME, SET_ACCENT, SET_VOLSTEP, SET_GROUPVOL, SET_ART, SET_CLOCK, SET_VOCAB, SET_PARTY,
#ifdef SONNIS_SOUND
	SET_SOUNDS,
#endif
	SET_ADD_IP, SET_CLEAR_IPS, SET_REDISCOVER, SET_DIAG, SET_NETLOG, SET_ABOUT, SET_QUIT, SET_COUNT
};

static void settings_row(int i, row_t *r, void *ctx)
{
	ctrl_state_t *st = ctx;
	settings_t *s = &g_settings;
	switch (i) {
	case SET_THEME: str_copy(r->title, "Theme", sizeof r->title);
		snprintf(r->right, sizeof r->right, "< %s >", THEMES[s->theme].name); break;
	case SET_ACCENT: str_copy(r->title, "Accent color", sizeof r->title);
		snprintf(r->right, sizeof r->right, "< %s >", ACCENTS[s->accent].name);
		r->has_dot = true; r->dot = C.accent; break;
	case SET_VOLSTEP: str_copy(r->title, "Volume step (L/R)", sizeof r->title);
		snprintf(r->right, sizeof r->right, "< %d >", s->volume_step); break;
	case SET_GROUPVOL: str_copy(r->title, "L/R controls group volume", sizeof r->title);
		str_copy(r->right, s->group_volume ? "On" : "Off", sizeof r->right); break;
	case SET_ART: str_copy(r->title, "Album artwork", sizeof r->title);
		str_copy(r->right, !s->show_art ? "Off" : s->vinyl ? "Vinyl" : "Square", sizeof r->right);
		if (s->show_art && s->vinyl) str_copy(r->subtitle, "Circle pad scratches the record", sizeof r->subtitle);
		break;
	case SET_VOCAB: str_copy(r->title, "Vocabulary", sizeof r->title);
		str_copy(r->right, s->ozzy ? "Prince of Darkness" : "Polite", sizeof r->right);
		if (s->ozzy) str_copy(r->subtitle, "Explicit language", sizeof r->subtitle);
		break;
	case SET_PARTY: {
		static const char *const N[PARTY_COUNT] = { "Auto (by song)", "Theme only", "Deadhead", "Disco lasers", "Sabbath" };
		str_copy(r->title, "Party mode", sizeof r->title);
		snprintf(r->right, sizeof r->right, "< %s >", N[s->party]);
		str_copy(r->subtitle, s->party == PARTY_AUTO ? "Effects follow the artist playing"
		                    : s->party == PARTY_THEME ? "Only the theme's own effects"
		                                              : "Forced on for every song", sizeof r->subtitle);
		break;
	}
	case SET_CLOCK: str_copy(r->title, "Clock", sizeof r->title);
		str_copy(r->right, s->clock_24h ? "24-hour" : "12-hour", sizeof r->right); break;
#ifdef SONNIS_SOUND
	case SET_SOUNDS: str_copy(r->title, "Ozzy sound bites", sizeof r->title);
		if (!sound_available()) str_copy(r->right, "No DSP firmware", sizeof r->right);
		else snprintf(r->right, sizeof r->right, "%d clip%s", sound_clip_count(), sound_clip_count() == 1 ? "" : "s");
		str_copy(r->subtitle, "WAVs in sdmc:/3ds/sonnis/sounds", sizeof r->subtitle);
		break;
#endif
	case SET_ADD_IP: str_copy(r->title, "Add speaker IP...", sizeof r->title);
		str_copy(r->subtitle, "For networks that block discovery", sizeof r->subtitle); break;
	case SET_CLEAR_IPS: str_copy(r->title, "Clear saved speaker IPs", sizeof r->title);
		snprintf(r->right, sizeof r->right, "%d saved", s->manual_count); break;
	case SET_REDISCOVER: str_copy(r->title, "Refresh discovery", sizeof r->title);
		snprintf(r->right, sizeof r->right, "%d found", st->speaker_count); break;
	case SET_DIAG: str_copy(r->title, "Network diagnostics...", sizeof r->title);
		str_copy(r->subtitle, "Test the connection to one speaker", sizeof r->subtitle); break;
	case SET_NETLOG: str_copy(r->title, "Network log", sizeof r->title);
		str_copy(r->subtitle, "Recent connection errors and retries", sizeof r->subtitle); break;
	case SET_ABOUT: snprintf(r->title, sizeof r->title, "Sonnis %s", APP_VERSION);
		snprintf(r->subtitle, sizeof r->subtitle, "This 2DS: %s", st->local_ip); r->dim = true; break;
	case SET_QUIT: str_copy(r->title, "Quit Sonnis", sizeof r->title); break;
	}
}

static int wrap(int v, int n) { return ((v % n) + n) % n; }

static void ask_speaker_ip(void)
{
	SwkbdState kb;
	char buf[20] = "";
	swkbdInit(&kb, SWKBD_TYPE_NUMPAD, 2, 15);
	swkbdSetNumpadKeys(&kb, L'.', 0);
	swkbdSetHintText(&kb, "Speaker IP, e.g. 192.168.1.40");
	swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	if (swkbdInputText(&kb, buf, sizeof buf) != SWKBD_BUTTON_CONFIRM) return;
	struct in_addr a;
	if (!inet_aton(buf, &a)) { ctrl_toast(true, "'%s' is not a valid IP address", buf); return; }
	settings_add_manual_ip(&g_settings, buf);
	settings_save(&g_settings);
	ctrl_toast(false, "Saved %s - searching...", buf);
	ctrl_send(CMD_DISCOVER, 0, 0, NULL);
}

static char diag_ip[16];

// Asks which speaker to test (pre-filled with a saved/known IP), then runs.
static void start_diag(bool ask)
{
	if (!diag_ip[0]) {
		if (g_settings.manual_count) str_copy(diag_ip, g_settings.manual_ips[0], sizeof diag_ip);
		ctrl_lock();
		const sonos_speaker_t *sp = ctrl_active_speaker(ctrl_state());
		if (!diag_ip[0] && sp) str_copy(diag_ip, sp->ip, sizeof diag_ip);
		ctrl_unlock();
	}
	if (ask) {
		SwkbdState kb;
		char buf[20];
		swkbdInit(&kb, SWKBD_TYPE_NUMPAD, 2, 15);
		swkbdSetNumpadKeys(&kb, L'.', 0);
		swkbdSetInitialText(&kb, diag_ip);
		swkbdSetHintText(&kb, "Speaker IP to test");
		swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
		if (swkbdInputText(&kb, buf, sizeof buf) != SWKBD_BUTTON_CONFIRM) return;
		struct in_addr a;
		if (!inet_aton(buf, &a)) { ctrl_toast(true, "'%s' is not a valid IP address", buf); return; }
		str_copy(diag_ip, buf, sizeof diag_ip);
	}
	ctrl_send(CMD_DIAG, 0, 0, diag_ip);
	screen = SCR_DIAG;
}

static void settings_change(int row, int dir) // dir: -1, +1, or 0 = activate
{
	settings_t *s = &g_settings;
	int d = dir ? dir : 1;
	switch (row) {
	case SET_THEME: s->theme = wrap(s->theme + d, THEME_COUNT); settings_apply_theme(s); break;
	case SET_ACCENT: s->accent = wrap(s->accent + d, ACCENT_COUNT); settings_apply_theme(s); break;
	case SET_VOLSTEP: s->volume_step = s->volume_step + d < 1 ? 10 : s->volume_step + d > 10 ? 1 : s->volume_step + d; break;
	case SET_GROUPVOL: s->group_volume = !s->group_volume; break;
	case SET_ART: { // Off -> Square -> Vinyl
		int v = wrap((!s->show_art ? 0 : s->vinyl ? 2 : 1) + d, 3);
		s->show_art = v != 0; // the worker notices on its next poll
		s->vinyl = v == 2;
		break;
	}
	case SET_VOCAB: s->ozzy = !s->ozzy; break;
	case SET_PARTY: s->party = wrap(s->party + d, PARTY_COUNT); break;
	case SET_ABOUT: { // tap 7 times to reveal Party mode
		if (dir) return; // left/right (e.g. during the Konami code) don't count
		static int taps;
		static u64 last;
		u64 now = osGetTime();
		if (s->party_unlocked) { ctrl_toast(false, "Party mode is already unlocked"); return; }
		taps = now - last < 1500 ? taps + 1 : 1;
		last = now;
		if (taps >= 7) {
			s->party_unlocked = true;
			ctrl_toast(false, "Party mode unlocked - it's in Settings now");
			if (s->ozzy) sound_play("bears");
			break; // saved with the other settings
		}
		if (taps >= 3) ctrl_toast(false, "%d more...", 7 - taps);
		return;
	}
	case SET_CLOCK: s->clock_24h = !s->clock_24h; break;
#ifdef SONNIS_SOUND
	case SET_SOUNDS:
		if (dir) return;
		sound_rescan();
		if (!sound_available()) ctrl_toast(true, "Audio needs sdmc:/3ds/dspfirm.cdc - run DSP1 once");
		else if (!s->ozzy) ctrl_toast(false, "Sound bites only play in Ozzy mode (Vocabulary)");
		else if (!sound_clip_count()) ctrl_toast(false, "No .wav files in sdmc:/3ds/sonnis/sounds yet");
		else if (!sound_play("start") && !sound_play("random")) ctrl_toast(false, "Found clips, but no start.wav or random*.wav");
		return;
#endif
	case SET_ADD_IP: if (!dir) ask_speaker_ip(); return;
	case SET_CLEAR_IPS: if (!dir) { s->manual_count = 0; ctrl_toast(false, "Cleared saved IPs"); } break;
	case SET_REDISCOVER: if (!dir) ctrl_send(CMD_DISCOVER, 0, 0, NULL); return;
	case SET_DIAG: if (!dir) start_diag(true); return;
	case SET_NETLOG: if (!dir) { screen = SCR_NETLOG; logserver_start(); } return;
	case SET_QUIT: {
		if (dir) return;
		static u64 armed;
		u64 now = osGetTime();
		if (now - armed < 3000) quit = true;
		else { armed = now; ctrl_toast(false, "Press again to quit"); }
		return;
	}
	default: return;
	}
	settings_dirty = true;
}

static void leave_settings(void)
{
	if (settings_dirty) { settings_save(&g_settings); settings_dirty = false; }
	screen = SCR_NOW;
}

// The Party mode row is hidden until unlocked: list positions skip it.
static int set_count(void) { return SET_COUNT - !g_settings.party_unlocked; }
static int set_row_id(int i) { return !g_settings.party_unlocked && i >= SET_PARTY ? i + 1 : i; }
static void settings_row_shown(int i, row_t *r, void *ctx) { settings_row(set_row_id(i), r, ctx); }

static void settings_input(const input_t *in)
{
	if ((in->down & KEY_B) || header_back_tapped(in)) { leave_settings(); return; }
	// Left/right adjust values here, so only use up/down for the list.
	input_t nav = *in;
	nav.repeat &= ~(KEY_LEFT | KEY_RIGHT);
	bool was_unlocked = g_settings.party_unlocked;
	int tapped = list_input(&nav, &set_cursor, &set_scroll, set_count(), HEADER_H, 180);
	int row = set_row_id(set_cursor);
	if (in->repeat & KEY_LEFT) settings_change(row, -1);
	if (in->repeat & KEY_RIGHT) settings_change(row, +1);
	if (in->down & KEY_A) settings_change(row, 0);
	if (tapped >= 0) {
		int id = set_row_id(tapped);
		bool is_value = id <= SET_PARTY;
		settings_change(id, is_value ? (in->sx < BOT_W / 2 ? -1 : +1) : 0);
	}
	// The new row appeared above the cursor: keep the cursor on About.
	if (!was_unlocked && g_settings.party_unlocked && set_cursor >= SET_PARTY) set_cursor++;
}

static void draw_settings(ctrl_state_t *st)
{
	draw_list(set_count(), set_cursor, set_scroll, HEADER_H, 180, settings_row_shown, st);
	draw_header("Settings", true, NULL);
	draw_footer_hint(G_LEFT G_RIGHT " change  " G_A " select  " G_B " save & back", NULL);
}

// ==== diagnostics ===============================================================

static void draw_diag_top(ctrl_state_t *st)
{
	C2D_TargetClear(top, C.bg);
	C2D_SceneBegin(top);
	rect(0, 0, TOP_W, 20, C.surface);
	text("Network diagnostics", 8, 3, 12, C.text, AL_LEFT, 0);
	if (st->diag_running) text(spinner(), TOP_W - 8, 3, 12, C.accent, AL_RIGHT, 0);
	for (int i = 0; i < st->diag_count; i++)
		text(st->diag[i], 6, 24 + i * 12.0f, 11, C.text, AL_LEFT, TOP_W - 12);
	if (st->diag_count == 0) text("Running...", 6, 24, 12, C.muted, AL_LEFT, 0);
}

static void diag_input(const input_t *in)
{
	if ((in->down & KEY_B) || header_back_tapped(in)) { screen = SCR_SETTINGS; return; }
	ctrl_lock();
	bool running = ctrl_state()->diag_running;
	ctrl_unlock();
	if (running) return;
	if ((in->down & KEY_A) || input_pressed_in(in, 40, 120, 240, 40)) start_diag(false);
	if ((in->down & KEY_X) || input_pressed_in(in, 40, 168, 240, 40)) start_diag(true);
}

static void draw_diag_bottom(const input_t *in, ctrl_state_t *st)
{
	draw_header("Diagnostics", true, NULL);
	text(st->diag_running ? "Testing... this takes up to 15 seconds" : "Results are on the top screen.",
	     BOT_W / 2.0f, 50, 13, C.text, AL_CENTER, BOT_W - 16);
	text("Saved to sdmc:/3ds/sonnis/debug.log", BOT_W / 2.0f, 74, 12, C.muted, AL_CENTER, BOT_W - 16);
	if (st->diag_running) return;
	rrect(40, 120, 240, 40, 10, touch_over(in, 40, 120, 240, 40) ? C.raised : C.surface);
	text(G_A " Run again", BOT_W / 2.0f, 131, 14, C.text, AL_CENTER, 230);
	rrect(40, 168, 240, 40, 10, touch_over(in, 40, 168, 240, 40) ? C.raised : C.surface);
	text(G_X " Test a different IP", BOT_W / 2.0f, 179, 14, C.text, AL_CENTER, 230);
}

// ==== network log ==============================================================

static void draw_netlog_top(void)
{
	C2D_TargetClear(top, C.bg);
	C2D_SceneBegin(top);
	rect(0, 0, TOP_W, 20, C.surface);
	text("Network log (newest at bottom)", 8, 3, 12, C.text, AL_LEFT, 0);
	static char lines[NETLOG_LINES][NETLOG_WIDTH];
	int n = netlog_copy(lines, NETLOG_LINES);
	if (n == 0) text("No network errors so far.", 6, 26, 12, C.muted, AL_LEFT, 0);
	for (int i = 0; i < n; i++) text(lines[i], 6, 23 + i * 12.0f, 11, C.text, AL_LEFT, TOP_W - 12);
}

static void netlog_input(const input_t *in)
{
	if ((in->down & KEY_B) || header_back_tapped(in)) { logserver_stop(); screen = SCR_SETTINGS; return; }
	if ((in->down & KEY_X) || input_pressed_in(in, 40, 170, 240, 40)) netlog_clear();
}

static void draw_netlog_bottom(const input_t *in, ctrl_state_t *st)
{
	draw_header("Network log", true, NULL);
	text("Errors and retries are listed on the top screen.", BOT_W / 2.0f, 42, 12, C.text, AL_CENTER, BOT_W - 16);
	text("'retry -> OK' = a hiccup the app recovered from.", BOT_W / 2.0f, 60, 12, C.muted, AL_CENTER, BOT_W - 16);

	rrect(12, 88, BOT_W - 24, 66, 10, C.surface);
	if (logserver_running()) {
		char url[64], hits[48];
		snprintf(url, sizeof url, "http://%s:%d/", st->local_ip, LOGSERVER_PORT);
		snprintf(hits, sizeof hits, "Sharing log on Wi-Fi - fetched %d time%s", logserver_hits(),
		         logserver_hits() == 1 ? "" : "s");
		text(hits, BOT_W / 2.0f, 96, 12, C.muted, AL_CENTER, BOT_W - 40);
		text(url, BOT_W / 2.0f, 116, 15, C.accent, AL_CENTER, BOT_W - 40);
#ifdef SONNIS_DEV
		text("Dev build: uploads on. Stops when you leave", BOT_W / 2.0f, 136, 11, C.muted, AL_CENTER, BOT_W - 40);
#else
		text("Stops when you leave this screen", BOT_W / 2.0f, 136, 11, C.muted, AL_CENTER, BOT_W - 40);
#endif
	} else {
		text("Log sharing unavailable", BOT_W / 2.0f, 112, 13, C.muted, AL_CENTER, BOT_W - 40);
	}
	rrect(40, 170, 240, 40, 10, touch_over(in, 40, 170, 240, 40) ? C.raised : C.surface);
	text(G_X " Clear log", BOT_W / 2.0f, 181, 14, C.text, AL_CENTER, 230);
}

// ==== frame =====================================================================

static void global_input(const input_t *in)
{
	if (screen == SCR_NETLOG && (in->down & (KEY_START | KEY_SELECT))) logserver_stop();
	if (in->down & KEY_SELECT) { if (screen == SCR_SETTINGS) leave_settings(); screen = SCR_NOW; }
	if (in->down & KEY_START) {
		if (screen == SCR_SETTINGS) leave_settings();
		else screen = SCR_SETTINGS;
	}
	int dv = 0;
	if (in->repeat & KEY_L) dv -= g_settings.volume_step;
	if (in->repeat & KEY_R) dv += g_settings.volume_step;
	if (dv) {
		ctrl_lock();
		int cur = ctrl_state()->volume;
		ctrl_unlock();
		if ((in->down & KEY_R) && cur >= 100) {
			ctrl_toast(false, "These go to eleven.");
			sound_play("eleven");
		}
		ctrl_set_volume(cur + dv);
	}
}

// Up Up Down Down Left Right Left Right B A toggles the bears. Works on any
// screen; Settings with the cursor on "About" is where the arrows do nothing
// else. Returns true when this press finished the B/A part, so the screen
// doesn't also treat it as Back/Select.
static bool konami(u32 down)
{
	static const u32 SEQ[] = { KEY_DUP, KEY_DUP, KEY_DDOWN, KEY_DDOWN, KEY_DLEFT,
	                           KEY_DRIGHT, KEY_DLEFT, KEY_DRIGHT, KEY_B, KEY_A };
	enum { N = sizeof SEQ / sizeof *SEQ };
	static int pos;
	down &= KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT | KEY_A | KEY_B;
	if (!down) return false;
	if (down != SEQ[pos]) {
		// A third Up still leaves us two Ups into the code.
		pos = down == KEY_DUP ? (pos == 2 ? 2 : 1) : 0;
		return false;
	}
	bool swallow = pos >= 8;
	if (++pos == N) {
		pos = 0;
		bears = !bears;
		ctrl_toast(false, bears ? "The bears are dancing" : "The bears went home");
		if (bears) sound_play("bears");
	}
	return swallow;
}

bool ui_frame(const input_t *in)
{
	frame_no++;
	g_ozzy = g_settings.ozzy;
	const input_t *raw = in;
	input_t filtered = *in;
	// A touch that opened this screen must not also count as a tap on it
	// (it used to land on "Quit" when Settings opened scrolled down).
	static bool touch_block;
	if (touch_block) {
		bool released = in->touch_up || !in->touching;
		filtered.touching = filtered.touch_down = filtered.touch_up = filtered.dragged = false;
		if (released) touch_block = false;
	}
	if (konami(in->down)) {
		filtered.down &= ~(KEY_A | KEY_B);
		filtered.repeat &= ~(KEY_A | KEY_B);
	}
	in = &filtered;
	theme_tick(fx_time());
	screen_t before = screen;
	global_input(in);
	if (screen == before) {
		switch (screen) {
		case SCR_NOW: now_input(in); break;
		case SCR_BROWSE: browse_input(in); break;
		case SCR_ROOMS: rooms_input(in); break;
		case SCR_SETTINGS: settings_input(in); break;
		case SCR_DIAG: diag_input(in); break;
		case SCR_NETLOG: netlog_input(in); break;
		}
	}
	if (screen != before && raw->touching) touch_block = true;

	// Persist a speaker IP the worker learned by scanning (file I/O stays on
	// this thread, which owns g_settings).
	char learned[16] = "";
	ctrl_lock();
	str_copy(learned, ctrl_state()->learned_ip, sizeof learned);
	ctrl_state()->learned_ip[0] = 0;
	ctrl_unlock();
	if (learned[0]) { settings_add_manual_ip(&g_settings, learned); settings_save(&g_settings); }

	ctrl_lock();
	ctrl_state_t *st = ctrl_state();
	sync_art(st);
	{
		const char *names[MAX_SPEAKERS];
		for (int i = 0; i < st->speaker_count; i++) names[i] = st->speakers[i].name;
		vox_set_rooms(names, st->speaker_count);
	}
	fx_mode_t song = st->have_status ? track_fx(&st->track) : FX_NONE;
	fx_mode_t theme_fx = C.fx ? FX_TIEDYE : FX_NONE;
	switch (g_settings.party_unlocked ? g_settings.party : PARTY_AUTO) {
	case PARTY_THEME: fx_mode = theme_fx; break;
	case PARTY_DEAD: fx_mode = FX_TIEDYE; break;
	case PARTY_LASER: fx_mode = FX_LASER; break;
	case PARTY_SABBATH: fx_mode = FX_SABBATH; break;
	default: fx_mode = song ? song : theme_fx; break;
	}

	// Sound-bite triggers: new toasts from the worker, and song changes.
	static u64 last_toast;
	static char last_title[128];
	const char *bite = NULL;
	if (st->toast_until != last_toast) {
		last_toast = st->toast_until;
		if (strstr(st->toast, "No next track")) bite = "lastsong";
		else if (st->toast_is_error) bite = "sharon";
	}
	if (st->have_status && strcmp(st->track.title, last_title)) {
		// Not on the very first status after connecting; then 1 in 3 songs.
		if (last_title[0] && st->track.title[0] && rand() % 3 == 0) bite = "random";
		str_copy(last_title, st->track.title, sizeof last_title);
	}

	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	font_frame_begin();
	if (screen == SCR_DIAG) draw_diag_top(st);
	else if (screen == SCR_NETLOG) draw_netlog_top();
	else draw_top(st);

	C2D_TargetClear(bot, C.bg);
	C2D_SceneBegin(bot);
	if (screen != SCR_DIAG && screen != SCR_NETLOG) draw_fx_bottom(screen == SCR_NOW);
	switch (screen) {
	case SCR_NOW: draw_now(in, st); break;
	case SCR_BROWSE: draw_browse(st); break;
	case SCR_ROOMS: draw_rooms(in, st); break;
	case SCR_SETTINGS: draw_settings(st); break;
	case SCR_DIAG: draw_diag_bottom(in, st); break;
	case SCR_NETLOG: draw_netlog_bottom(in, st); break;
	}
	ctrl_unlock();
	C3D_FrameEnd(0);
	if (bite && !sound_playing()) sound_play(bite); // file I/O outside the lock
	return !quit;
}

void ui_init(void)
{
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 2);
	C2D_Prepare();
	top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	font_init();

	C3D_TexInit(&art_tex, ART_SIZE, ART_SIZE, GPU_RGBA8);
	C3D_TexSetFilter(&art_tex, GPU_LINEAR, GPU_LINEAR);
	for (int i = 0; i < 2; i++) {
		C3D_TexInit(&vinyl_tex[i], ART_SIZE, ART_SIZE, GPU_RGBA8);
		memset(vinyl_tex[i].data, 0, ART_SIZE * ART_SIZE * 4);
		C3D_TexFlush(&vinyl_tex[i]);
		C3D_TexSetFilter(&vinyl_tex[i], GPU_LINEAR, GPU_LINEAR);
	}
}

void ui_exit(void)
{
	logserver_stop();
	C3D_TexDelete(&art_tex);
	for (int i = 0; i < 2; i++) C3D_TexDelete(&vinyl_tex[i]);
	free(art_copy);
	art_copy = NULL;
	font_exit();
	C2D_Fini();
	C3D_Fini();
}
