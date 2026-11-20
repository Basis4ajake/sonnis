// settings: persistent configuration in sdmc:/3ds/sonnis/settings.ini plus a
// small "recently played" history in sdmc:/3ds/sonnis/history.txt.
#pragma once
#include <stdbool.h>
#include <3ds/types.h>
#include "sonos_library.h"
#include "ui_theme.h"

#define SETTINGS_DIR "sdmc:/3ds/sonnis"
#define MAX_PREFERRED 8
#define MAX_MANUAL_IPS 4
#define MAX_HISTORY 15

// Party mode: which animated effect to show, regardless of the song.
enum { PARTY_AUTO, PARTY_THEME, PARTY_DEAD, PARTY_LASER, PARTY_SABBATH, PARTY_COUNT };

typedef struct {
	// connection
	char last_room_uuid[48];
	char preferred[MAX_PREFERRED][48]; // pinned rooms, shown first
	int preferred_count;
	char manual_ips[MAX_MANUAL_IPS][16]; // used when SSDP finds nothing
	int manual_count;

	// behaviour
	int volume_step;      // 1..10
	bool show_art;
	bool vinyl;           // draw album art as a spinning record
	bool ozzy;            // explicit rock-and-roll vocabulary
	bool party_unlocked;  // hidden "Party mode" row revealed (tap About 7x)
	int party;            // PARTY_* : song effects override
	bool clock_24h;
	bool group_volume;    // L/R change whole group when grouped

	// personalization
	int theme;            // index into THEMES
	int accent;           // index into ACCENTS
	u32 color_override[TC_COUNT];
	u32 color_override_mask;
} settings_t;

extern settings_t g_settings;

void settings_defaults(settings_t *s);
bool settings_load(settings_t *s);
bool settings_save(const settings_t *s);
void settings_apply_theme(const settings_t *s);

bool settings_is_preferred(const settings_t *s, const char *uuid);
void settings_toggle_preferred(settings_t *s, const char *uuid);
bool settings_add_manual_ip(settings_t *s, const char *ip);

// Recently played (stored locally; the Sonos local API has no history).
void history_load(lib_list_t *out);
void history_add(const lib_item_t *it);
