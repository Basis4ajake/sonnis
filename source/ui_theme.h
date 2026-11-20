// ui_theme: color palettes. Colors are 0xRRGGBB here and converted to
// citro2d's ABGR format once, when a theme is applied.
// Preset 0 ("Midnight") mirrors the Claude Design canvas tokens.
#pragma once
#include <3ds/types.h>
#include <stdbool.h>

typedef struct {
	const char *name;
	u32 bg, surface, raised, line, text, muted, accent, ok, err;
	bool psychedelic; // animated tie-dye effects
} theme_t;

typedef struct {
	const char *name;
	u32 rgb; // 0 = use the theme's own accent
} accent_t;

// Indices into the color slots, used by settings for "color.<slot>" overrides.
enum { TC_BG, TC_SURFACE, TC_RAISED, TC_LINE, TC_TEXT, TC_MUTED, TC_ACCENT, TC_OK, TC_ERR, TC_COUNT };
extern const char *const THEME_SLOT_NAMES[TC_COUNT];

extern const theme_t THEMES[];
extern const int THEME_COUNT;
extern const accent_t ACCENTS[];
extern const int ACCENT_COUNT;

// Resolved, ready-to-draw colors (citro2d u32).
typedef struct {
	u32 bg, surface, raised, line, text, muted, accent, on_accent, ok, err, overlay;
	bool fx;          // theme draws animated effects
	bool fx_cycle;    // accent hue cycles (fx theme with default accent)
} ui_colors_t;

extern ui_colors_t C; // the active palette, read by all drawing code

// Per-frame animation (cycling accent). t = seconds.
void theme_tick(float t);

// Rainbow color for a hue in [0,1) (wraps), as a citro2d color.
u32 theme_hue(float h, float s, float v, u8 alpha);

// Applies preset + accent + per-slot overrides (override_mask bit i => use overrides[i]).
void theme_apply(int theme_idx, int accent_idx, u32 override_mask, const u32 overrides[TC_COUNT]);
