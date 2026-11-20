// ozzy: the "Prince of Darkness" vocabulary (Settings > Vocabulary).
// Every UI string drawn with text() goes through vox(); when the mode is on,
// known strings are swapped for rock-and-roll (explicit) versions.
#pragma once
#include <stdbool.h>
#include <stddef.h>

extern bool g_ozzy;

// Returns s unchanged, or a rewritten copy in buf (cap bytes).
// Table keys: "exact", "prefix *" (rest of s is appended) or
// "* suffix" (start of s is kept).
const char *vox(const char *s, char *buf, size_t cap);

// Room names to swear at: in Ozzy mode every whole-word occurrence becomes
// "Fucking <name>" or "Bloody <name>" (fixed per room). Call once a frame.
void vox_set_rooms(const char *const *names, int n);
