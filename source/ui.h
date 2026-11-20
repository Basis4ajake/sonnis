// ui: dual-screen interface. Top screen = Now Playing (always); bottom
// screen = controls, browse lists, rooms and settings.
#pragma once
#include <stdbool.h>
#include "input.h"

void ui_init(void);
void ui_exit(void);

// Handles one frame of input and draws both screens.
// Returns false when the user chose to quit.
bool ui_frame(const input_t *in);
