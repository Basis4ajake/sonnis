// sonos_discovery: finds speakers with SSDP, then asks one of them for the
// household's zone-group topology, which lists every room, its IP and which
// group (and group coordinator) it belongs to.
#pragma once
#include <stdbool.h>

#define MAX_SPEAKERS 32

typedef struct {
	char uuid[48];             // RINCON_xxxxxxxxxxxx01400
	char name[64];             // room name, e.g. "Living Room"
	char ip[16];
	char group_id[64];
	char coordinator_uuid[48]; // uuid of the group's coordinator
	bool is_coordinator;
	bool invisible;            // bonded satellites / subs: hidden from room list
	bool reachable;
	int group_size;            // number of visible members in this speaker's group
} sonos_speaker_t;

// Sends SSDP M-SEARCH and collects unique responder IPs. Returns count.
int sonos_ssdp_search(char ips[][16], int max_ips, int timeout_ms);

// Fallback for networks that filter multicast: probes TCP port 1400 across
// the local /24 (local_ip in network byte order), a batch of sockets at a
// time. Stops after `max_ips` hits. Returns count. Takes ~5-7 s worst case.
int sonos_subnet_scan(unsigned long local_ip, char ips[][16], int max_ips);

// Fetches the zone-group topology from the speaker at `ip`. Returns the number
// of speakers written to `out`, or a negative HTTP_ERR_* code.
int sonos_fetch_topology(const char *ip, sonos_speaker_t *out, int max);

// Convenience: index of the coordinator for speakers[i], or i itself.
int sonos_find_coordinator(const sonos_speaker_t *s, int count, int i);
