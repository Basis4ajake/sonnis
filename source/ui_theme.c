#include "ui_theme.h"

#include <citro2d.h>
#include <math.h>

const char *const THEME_SLOT_NAMES[TC_COUNT] = {
	"bg", "surface", "raised", "line", "text", "muted", "accent", "ok", "err",
};

const theme_t THEMES[] = {
	//  name        bg        surface   raised    line      text      muted     accent    ok        err
	{ "Midnight", 0x0E0E10, 0x1B1B1F, 0x2A2A30, 0x34343A, 0xF4F4F5, 0xA1A1AA, 0xF2B544, 0x4ADE80, 0xF87171, false },
	{ "Mono",     0x000000, 0x141414, 0x242424, 0x333333, 0xFFFFFF, 0x9E9E9E, 0xFFFFFF, 0x4ADE80, 0xF87171, false },
	{ "Ocean",    0x0B1320, 0x13213A, 0x1C2E4D, 0x2A3F63, 0xEAF2FF, 0x9DB0CC, 0x4FC3F7, 0x5EEAD4, 0xFCA5A5, false },
	{ "Forest",   0x0E1511, 0x17231C, 0x213328, 0x2E4536, 0xEEF5EF, 0x9CB3A3, 0x7BD88F, 0xA3E635, 0xFCA5A5, false },
	{ "Plum",     0x140E17, 0x221828, 0x30223A, 0x3E2D4A, 0xF5EEF8, 0xB7A6C2, 0xE879F9, 0x86EFAC, 0xFDA4AF, false },
	{ "Daylight", 0xF4F4F5, 0xFFFFFF, 0xE4E4E7, 0xD4D4D8, 0x18181B, 0x52525B, 0xC2410C, 0x15803D, 0xB91C1C, false },
	{ "Handheld", 0x0F380F, 0x1E4D1E, 0x306230, 0x3E733E, 0x9BBC0F, 0x8BAC0F, 0xC4E538, 0x9BBC0F, 0xE0F870, false },
	// Tie-dye night: deep indigo, warm cream type, rainbow everything.
	{ "Deadhead", 0x140A24, 0x22123A, 0x34205A, 0x4A2F75, 0xFFF4E0, 0xCDB8EA, 0xFF4E50, 0x7CFF6B, 0xFF7A7A, true },
};
const int THEME_COUNT = sizeof THEMES / sizeof *THEMES;

const accent_t ACCENTS[] = {
	{ "Theme default", 0 },
	{ "Amber",  0xF2B544 },
	{ "Coral",  0xFB7185 },
	{ "Sky",    0x38BDF8 },
	{ "Mint",   0x34D399 },
	{ "Violet", 0xA78BFA },
	{ "Gold",   0xFACC15 },
	{ "White",  0xF4F4F5 },
};
const int ACCENT_COUNT = sizeof ACCENTS / sizeof *ACCENTS;

ui_colors_t C;

static u32 to_c2d(u32 rgb, u8 a)
{
	return C2D_Color32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a);
}

// Picks black or white text for legibility on top of the accent color,
// using the WCAG relative-luminance midpoint.
static u32 contrast_on(u32 rgb)
{
	float r = ((rgb >> 16) & 0xFF) / 255.0f, g = ((rgb >> 8) & 0xFF) / 255.0f, b = (rgb & 0xFF) / 255.0f;
	float lum = 0.2126f * r * r + 0.7152f * g * g + 0.0722f * b * b; // gamma ~2 approximation
	return lum > 0.18f ? 0x111113 : 0xFFFFFF;
}

u32 theme_hue(float h, float s, float v, u8 a)
{
	h -= floorf(h);
	float r, g, b, f = h * 6.0f;
	int i = (int)f;
	f -= i;
	float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
	switch (i % 6) {
	case 0: r = v; g = t; b = p; break;
	case 1: r = q; g = v; b = p; break;
	case 2: r = p; g = v; b = t; break;
	case 3: r = p; g = q; b = v; break;
	case 4: r = t; g = p; b = v; break;
	default: r = v; g = p; b = q; break;
	}
	return C2D_Color32((u8)(r * 255), (u8)(g * 255), (u8)(b * 255), a);
}

void theme_tick(float t)
{
	if (!C.fx_cycle) return;
	// One trip around the color wheel every ~20 s; slightly desaturated so
	// it stays readable as a button fill. Text on it is dark at this value.
	C.accent = theme_hue(t / 20.0f, 0.72f, 1.0f, 0xFF);
	C.on_accent = C2D_Color32(0x14, 0x0A, 0x24, 0xFF);
}

void theme_apply(int theme_idx, int accent_idx, u32 mask, const u32 ov[TC_COUNT])
{
	if (theme_idx < 0 || theme_idx >= THEME_COUNT) theme_idx = 0;
	if (accent_idx < 0 || accent_idx >= ACCENT_COUNT) accent_idx = 0;
	const theme_t *t = &THEMES[theme_idx];

	u32 slot[TC_COUNT] = { t->bg, t->surface, t->raised, t->line, t->text, t->muted, t->accent, t->ok, t->err };
	if (ACCENTS[accent_idx].rgb) slot[TC_ACCENT] = ACCENTS[accent_idx].rgb;
	for (int i = 0; i < TC_COUNT; i++)
		if (mask & (1u << i)) slot[i] = ov[i];

	C.bg = to_c2d(slot[TC_BG], 0xFF);
	C.surface = to_c2d(slot[TC_SURFACE], 0xFF);
	C.raised = to_c2d(slot[TC_RAISED], 0xFF);
	C.line = to_c2d(slot[TC_LINE], 0xFF);
	C.text = to_c2d(slot[TC_TEXT], 0xFF);
	C.muted = to_c2d(slot[TC_MUTED], 0xFF);
	C.accent = to_c2d(slot[TC_ACCENT], 0xFF);
	C.on_accent = to_c2d(contrast_on(slot[TC_ACCENT]), 0xFF);
	C.ok = to_c2d(slot[TC_OK], 0xFF);
	C.err = to_c2d(slot[TC_ERR], 0xFF);
	C.overlay = to_c2d(slot[TC_BG], 0xD8);
	C.fx = t->psychedelic;
	C.fx_cycle = t->psychedelic && ACCENTS[accent_idx].rgb == 0 && !(mask & (1u << TC_ACCENT));
}
