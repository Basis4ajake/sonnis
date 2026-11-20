// ui_font: crisp text. Inter is pre-rendered (mkbcfnt) at each exact pixel
// size the UI uses and drawn at scale 1.0, so glyphs map 1:1 to screen
// pixels. Characters Inter lacks (3DS button glyphs, CJK, etc.) fall back to
// the system font run-by-run, baseline-aligned, inside the same string.
#pragma once
#include <citro2d.h>

void font_init(void);
void font_exit(void);

// Width in pixels of s rendered at `px`.
float font_measure(const char *s, float px);

// Draws s with its top-left at (x, y) (positions are snapped to whole pixels).
// Returns the width drawn.
float font_draw(const char *s, float x, float y, float px, u32 color);

// Call once per frame before drawing (clears the glyph buffers).
void font_frame_begin(void);
