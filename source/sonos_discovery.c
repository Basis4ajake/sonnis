#define _GNU_SOURCE // strcasestr
#include "sonos_discovery.h"
#include "sonos_http.h"
#include "xml_util.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *MSEARCH =
    "M-SEARCH * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "MAN: \"ssdp:discover\"\r\n"
    "MX: 1\r\n"
    "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n"
    "\r\n";

int sonos_ssdp_search(char ips[][16], int max_ips, int timeout_ms)
{
	int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0) return 0;

	// No bind(): on the 3DS binding to port 0 fails (seen in on-device diag);
	// sendto() assigns an ephemeral port by itself.

	struct sockaddr_in mc = { .sin_family = AF_INET, .sin_port = htons(1900) };
	inet_aton("239.255.255.250", &mc.sin_addr);

	int found = 0;
	char buf[1536];
	u64 start = osGetTime();
	u64 next_send = start;
	int sends = 0;

	while ((int)(osGetTime() - start) < timeout_ms && found < max_ips && !net_aborting()) {
		// UDP is lossy and Wi-Fi multicast doubly so: send the probe 3 times.
		if (sends < 3 && osGetTime() >= next_send) {
			sendto(fd, MSEARCH, strlen(MSEARCH), 0, (struct sockaddr *)&mc, sizeof mc);
			sends++;
			next_send = osGetTime() + 400;
		}
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		if (poll(&pfd, 1, 100) <= 0) continue;

		struct sockaddr_in from; socklen_t fl = sizeof from;
		ssize_t n = recvfrom(fd, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
		if (n <= 0) continue;
		buf[n] = 0;
		// Only Sonos players answer with this server token / ST.
		if (!strcasestr(buf, "Sonos") && !strcasestr(buf, "ZonePlayer")) continue;

		const char *ip = inet_ntoa(from.sin_addr);
		bool dup = false;
		for (int i = 0; i < found; i++) if (!strcmp(ips[i], ip)) dup = true;
		if (!dup) str_copy(ips[found++], ip, 16);
	}
	close(fd);
	return found;
}

#define SCAN_BATCH 16      // the 3DS socket service has a small fd budget
#define SCAN_WAIT_MS 1200  // 2DS Wi-Fi: ARP + high RTT + sleepy speakers

int sonos_subnet_scan(unsigned long local_ip, char ips[][16], int max_ips)
{
	u32 host = ntohl((u32)local_ip);
	u32 base = host & 0xFFFFFF00u;
	int found = 0;

	for (u32 start = 1; start < 255 && found < max_ips && !net_aborting(); start += SCAN_BATCH) {
		int fds[SCAN_BATCH];
		u32 addrs[SCAN_BATCH];
		int n = 0;
		for (u32 h = start; h < start + SCAN_BATCH && h < 255; h++) {
			u32 a = base | h;
			if (a == host) continue;
			int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
			if (fd < 0) break;
			fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
			struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(SONOS_PORT), .sin_addr.s_addr = htonl(a) };
			connect(fd, (struct sockaddr *)&sa, sizeof sa); // EINPROGRESS expected
			fds[n] = fd;
			addrs[n++] = a;
		}

		// Collect completions until the batch deadline. poll() just wakes us;
		// sock_connect_status() decides (see sonos_http.c for why).
		bool done[SCAN_BATCH] = { 0 };
		struct sockaddr_in sas[SCAN_BATCH];
		for (int i = 0; i < n; i++) {
			memset(&sas[i], 0, sizeof sas[i]);
			sas[i].sin_family = AF_INET;
			sas[i].sin_port = htons(SONOS_PORT);
			sas[i].sin_addr.s_addr = htonl(addrs[i]);
		}
		u64 deadline = osGetTime() + SCAN_WAIT_MS;
		int pending = n;
		while (pending > 0 && found < max_ips && !net_aborting()) {
			int left = (int)(deadline - osGetTime());
			if (left <= 0) break;
			struct pollfd p[SCAN_BATCH];
			int np = 0;
			for (int i = 0; i < n; i++)
				if (!done[i]) { p[np].fd = fds[i]; p[np].events = POLLOUT; p[np].revents = 0; np++; }
			poll(p, np, left > 150 ? 150 : left);
			for (int i = 0; i < n && found < max_ips; i++) {
				if (done[i]) continue;
				int st = sock_connect_status(fds[i], &sas[i]);
				if (st == 0) continue;
				done[i] = true;
				pending--;
				if (st == 1) {
					struct in_addr ia = { .s_addr = htonl(addrs[i]) };
					str_copy(ips[found++], inet_ntoa(ia), 16);
				}
			}
		}
		for (int i = 0; i < n; i++) close(fds[i]);
	}
	return found;
}

int sonos_find_coordinator(const sonos_speaker_t *s, int count, int i)
{
	for (int j = 0; j < count; j++)
		if (!strcmp(s[j].uuid, s[i].coordinator_uuid)) return j;
	return i;
}

int sonos_fetch_topology(const char *ip, sonos_speaker_t *out, int max)
{
	http_response_t r;
	int rc = soap_call(ip, "/ZoneGroupTopology/Control",
	                   "urn:schemas-upnp-org:service:ZoneGroupTopology:1",
	                   "GetZoneGroupState", "", &r, NULL);
	if (rc != HTTP_OK) { http_response_free(&r); return rc; }

	// The state document is XML escaped inside the SOAP envelope. Unescape it
	// in place (escaped text is always longer, so this is safe).
	const char *c; size_t n;
	if (!xml_find(r.body, "ZoneGroupState", NULL, &c, &n, NULL)) { http_response_free(&r); return HTTP_ERR_PROTO; }
	char *state = (char *)c;
	state[n] = 0;
	xml_unescape(state);

	int count = 0;
	const char *p = state, *grp_open, *grp_c, *grp_after;
	size_t grp_n;
	while (count < max && xml_find(p, "ZoneGroup", &grp_open, &grp_c, &grp_n, &grp_after)) {
		char coord[48], gid[64];
		xml_attr(grp_open, "Coordinator", coord, sizeof coord);
		xml_attr(grp_open, "ID", gid, sizeof gid);

		// Iterate members only inside this group's content.
		char saved = grp_c[grp_n];
		((char *)grp_c)[grp_n] = 0;
		const char *m = grp_c, *mopen, *mafter;
		int first_in_group = count;
		while (count < max && xml_find(m, "ZoneGroupMember", &mopen, NULL, NULL, &mafter)) {
			sonos_speaker_t *s = &out[count];
			memset(s, 0, sizeof *s);
			char loc[128], invis[8], host[32], path[64];
			int port;
			xml_attr(mopen, "UUID", s->uuid, sizeof s->uuid);
			xml_attr(mopen, "ZoneName", s->name, sizeof s->name);
			xml_attr(mopen, "Location", loc, sizeof loc);
			xml_attr(mopen, "Invisible", invis, sizeof invis);
			if (http_parse_url(loc, host, sizeof host, &port, path, sizeof path))
				str_copy(s->ip, host, sizeof s->ip);
			str_copy(s->group_id, gid, sizeof s->group_id);
			str_copy(s->coordinator_uuid, coord, sizeof s->coordinator_uuid);
			s->is_coordinator = !strcmp(s->uuid, coord);
			s->invisible = invis[0] == '1';
			s->reachable = true;
			if (s->ip[0]) count++;
			m = mafter;
		}
		((char *)grp_c)[grp_n] = saved;

		int visible = 0;
		for (int i = first_in_group; i < count; i++) if (!out[i].invisible) visible++;
		for (int i = first_in_group; i < count; i++) out[i].group_size = visible;
		p = grp_after;
	}
	http_response_free(&r);
	return count;
}
