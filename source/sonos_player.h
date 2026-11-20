// sonos_player: transport, volume, play-mode, queue and grouping control via
// the speaker's local UPnP services. Transport/queue calls must target the
// GROUP COORDINATOR; volume calls target the individual room.
#pragma once
#include <stdbool.h>

typedef enum { TS_UNKNOWN, TS_STOPPED, TS_PLAYING, TS_PAUSED, TS_TRANSITIONING } transport_state_t;
typedef enum { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE } repeat_mode_t;

typedef struct {
	char title[128];
	char artist[96];
	char album[96];
	char art_uri[512];   // as reported (often relative "/getaa?...")
	int track_num;       // 1-based position in queue, 0 if not from queue
	int elapsed_s;
	int duration_s;      // 0 for radio streams
	bool is_stream;
} track_info_t;

int sonos_play(const char *ip);
int sonos_pause(const char *ip);
int sonos_next(const char *ip);
int sonos_previous(const char *ip);
int sonos_seek_time(const char *ip, int seconds);
int sonos_seek_track(const char *ip, int track_1based);

int sonos_get_transport_state(const char *ip, transport_state_t *out);
int sonos_get_position(const char *ip, track_info_t *out);

int sonos_get_play_mode(const char *ip, bool *shuffle, repeat_mode_t *repeat);
int sonos_set_play_mode(const char *ip, bool shuffle, repeat_mode_t repeat);
const char *sonos_playmode_to_string(bool shuffle, repeat_mode_t repeat);
void sonos_playmode_from_string(const char *s, bool *shuffle, repeat_mode_t *repeat);

int sonos_get_volume(const char *ip, int *vol);
int sonos_set_volume(const char *ip, int vol);
int sonos_get_mute(const char *ip, bool *muted);
int sonos_set_mute(const char *ip, bool muted);
int sonos_get_group_volume(const char *coord_ip, int *vol);
int sonos_set_group_volume(const char *coord_ip, int vol);

int sonos_set_av_uri(const char *ip, const char *uri, const char *meta);
int sonos_add_to_queue(const char *ip, const char *uri, const char *meta, int *first_track);
int sonos_clear_queue(const char *ip);
int sonos_remove_from_queue(const char *ip, int track_1based);
int sonos_use_queue(const char *coord_ip, const char *coord_uuid);

// Grouping. Join: `member_ip` starts following `coordinator_uuid`.
int sonos_join_group(const char *member_ip, const char *coordinator_uuid);
int sonos_delegate_coordination(const char *coordinator_ip, const char *new_coordinator_uuid, bool rejoin);
int sonos_leave_group(const char *member_ip);

// Parses "H:MM:SS" into seconds (returns 0 for NOT_IMPLEMENTED etc.).
int sonos_parse_time(const char *s);
