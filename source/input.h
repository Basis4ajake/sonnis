// input: one snapshot of buttons + touch per frame, with tap/drag detection.
#pragma once
#include <stdbool.h>
#include <3ds/types.h>

typedef struct {
	u32 down;       // pressed this frame
	u32 held;
	u32 repeat;     // down + auto-repeat while held (for lists / volume)

	bool touching;  // stylus on screen
	bool touch_down;     // first frame of contact
	bool touch_up;       // first frame after release
	bool dragged;        // moved > threshold since touch_down
	int tx, ty;          // current (or last) position
	int sx, sy;          // where the touch started
	int dy_frame;        // vertical movement since previous frame
} input_t;

void input_init(void);
void input_poll(input_t *in);

// True on the frame the stylus first lands inside the rectangle.
bool input_pressed_in(const input_t *in, int x, int y, int w, int h);
// True while the stylus is held and the touch STARTED inside the rectangle.
bool input_holding_in(const input_t *in, int x, int y, int w, int h);
// True on release when the touch started inside and was not a drag (a tap).
bool input_tapped_in(const input_t *in, int x, int y, int w, int h);
