#include "input.h"

#include <3ds.h>
#include <stdlib.h>

#define DRAG_THRESHOLD 6

static bool was_touching;
static int last_ty;

void input_init(void)
{
	// 20 frames before auto-repeat kicks in, then every 5 frames (12/s).
	hidSetRepeatParameters(20, 5);
}

void input_poll(input_t *in)
{
	hidScanInput();
	in->down = hidKeysDown();
	in->held = hidKeysHeld();
	in->repeat = hidKeysDownRepeat();

	bool touching = (in->held & KEY_TOUCH) != 0;
	in->touch_down = touching && !was_touching;
	in->touch_up = !touching && was_touching;
	in->dy_frame = 0;

	if (touching) {
		touchPosition tp;
		hidTouchRead(&tp);
		in->tx = tp.px;
		in->ty = tp.py;
		if (in->touch_down) {
			in->sx = in->tx; in->sy = in->ty;
			in->dragged = false;
			last_ty = in->ty;
		}
		if (abs(in->tx - in->sx) > DRAG_THRESHOLD || abs(in->ty - in->sy) > DRAG_THRESHOLD) in->dragged = true;
		in->dy_frame = in->ty - last_ty;
		last_ty = in->ty;
	}
	in->touching = touching;
	was_touching = touching;
}

static bool inside(int px, int py, int x, int y, int w, int h)
{
	return px >= x && px < x + w && py >= y && py < y + h;
}

bool input_pressed_in(const input_t *in, int x, int y, int w, int h)
{
	return in->touch_down && inside(in->tx, in->ty, x, y, w, h);
}

bool input_holding_in(const input_t *in, int x, int y, int w, int h)
{
	return in->touching && inside(in->sx, in->sy, x, y, w, h);
}

bool input_tapped_in(const input_t *in, int x, int y, int w, int h)
{
	return in->touch_up && !in->dragged && inside(in->sx, in->sy, x, y, w, h);
}
