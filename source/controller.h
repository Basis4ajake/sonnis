// controller: the background worker that owns ALL network I/O.
//
// The UI thread never touches a socket. It posts commands (ctrl_send) and
// reads a shared snapshot (ctrl_state) under a lock. The worker executes
// commands, polls the active room about once a second, refreshes topology,
// fetches album art and handles reconnects — so the UI keeps 60 fps even when
// a speaker takes seconds to answer or vanishes entirely.
#pragma once
#include <stdbool.h>
#include <3ds/types.h>
#include "network.h"
#include "sonos_discovery.h"
#include "sonos_library.h"
#include "sonos_player.h"

typedef enum {
	CMD_DISCOVER,
	CMD_SELECT_ROOM,     // s = uuid
	CMD_PLAY_PAUSE,
	CMD_NEXT,
	CMD_PREV,
	CMD_MUTE_TOGGLE,
	CMD_SHUFFLE_TOGGLE,
	CMD_REPEAT_CYCLE,
	CMD_BROWSE,          // s = object id, a = start index (0 = fresh list)
	CMD_PLAY_ITEM,       // a = index in current browse list, b = browse_gen
	CMD_QUEUE_REMOVE,    // a = index in queue list
	CMD_QUEUE_CLEAR,
	CMD_JOIN,            // s = uuid of room that should join the active group
	CMD_LEAVE,           // s = uuid of room that should become standalone
	CMD_DIAG,            // s = speaker IP to run network diagnostics against
	CMD_SEEK,            // a = position in seconds
} cmd_type_t;

typedef struct {
	// network
	net_status_t net;
	char local_ip[16];

	// speakers / rooms
	bool discovering;
	bool scanning;         // SSDP failed; probing the subnet
	char learned_ip[16];   // found by scan; UI thread saves it to settings
	bool discovered_once;
	sonos_speaker_t speakers[MAX_SPEAKERS];
	int speaker_count;
	char active_uuid[48];  // selected room ("" = none)
	bool reachable;        // last poll of the active room succeeded

	// playback (of the active room's group)
	bool have_status;
	transport_state_t ts;
	track_info_t track;
	u64 track_tick;        // osGetTime() when track.elapsed_s was sampled
	bool shuffle;
	repeat_mode_t repeat;
	int volume;            // room or group volume, 0..100
	bool muted;
	bool vol_dirty;        // UI changed volume; worker will push it

	// album art (ART_SIZE^2 RGBA, owned by the worker)
	u32 *art_pixels;
	int art_gen;           // bumps whenever art_pixels changes (NULL = no art)

	// browsing
	lib_list_t browse;
	char browse_id[160];
	bool browse_loading;
	int browse_gen;        // bumps when a fresh list starts
	int browse_err;

	// network diagnostics output (shown on the top screen)
	char diag[18][96];
	int diag_count;
	bool diag_running;

	// transient message for the user
	char toast[128];
	u64 toast_until;
	bool toast_is_error;
} ctrl_state_t;

void ctrl_start(void);
// Stops the worker. Returns false if it did not stop in time; the caller must
// then NOT shut down the socket service (the worker may still be using it).
bool ctrl_stop(void);

void ctrl_lock(void);
void ctrl_unlock(void);
ctrl_state_t *ctrl_state(void); // only dereference between lock/unlock

void ctrl_send(cmd_type_t type, int a, int b, const char *s);
void ctrl_set_volume(int vol);   // coalesced: only the latest value is sent
void ctrl_on_wake(void);         // console woke from sleep: re-check network
void ctrl_toast(bool error, const char *fmt, ...);

// Helpers that read the state (call with the lock held).
const sonos_speaker_t *ctrl_active_speaker(const ctrl_state_t *st);
int ctrl_estimated_elapsed(const ctrl_state_t *st);
