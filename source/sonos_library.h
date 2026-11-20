// sonos_library: ContentDirectory browsing (favorites, playlists, queue,
// local music library, radio) and "play this item" logic.
#pragma once
#include <stdbool.h>

// Well-known ContentDirectory object IDs on Sonos players.
#define LIB_ID_FAVORITES  "FV:2"
#define LIB_ID_PLAYLISTS  "SQ:"
#define LIB_ID_QUEUE      "Q:0"
#define LIB_ID_ARTISTS    "A:ALBUMARTIST"
#define LIB_ID_ALBUMS     "A:ALBUM"
#define LIB_ID_TRACKS     "A:TRACKS"
#define LIB_ID_GENRES     "A:GENRE"
#define LIB_ID_RADIO      "R:0/0"
// Pseudo IDs handled locally (never sent to a speaker).
#define LIB_ID_HISTORY    "@history"
#define LIB_ID_SERVICES   "@services"

typedef struct {
	char id[160];
	char title[96];
	char subtitle[80];
	char uri[512];
	char *meta;         // DIDL-Lite metadata to send when playing (malloc'd), may be NULL
	bool container;     // can be opened
	bool playable;      // has a resource URI
	bool info_only;     // informational row (e.g. "needs cloud login")
} lib_item_t;

typedef struct {
	lib_item_t *items;
	int count;
	int capacity;
	int total;          // total matches reported by the speaker
} lib_list_t;

// Appends up to `count` children of object_id starting at `start`.
int sonos_browse(const char *ip, const char *object_id, int start, int count, lib_list_t *out);

lib_item_t *lib_list_push(lib_list_t *l);
void lib_list_clear(lib_list_t *l);
void lib_item_free(lib_item_t *it);
void lib_item_copy(lib_item_t *dst, const lib_item_t *src);

// Plays an item on the group whose coordinator is (ip, uuid).
// parent_id + index identify where the item came from (needed for the queue).
int sonos_play_item(const char *coord_ip, const char *coord_uuid,
                    const lib_item_t *it, const char *parent_id, int index);

// Fills `out` with the static "Music services" explainer rows.
void sonos_list_services_info(lib_list_t *out);
