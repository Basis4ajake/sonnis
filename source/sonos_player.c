#include "sonos_player.h"
#include "sonos_http.h"
#include "xml_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AVT_PATH "/MediaRenderer/AVTransport/Control"
#define AVT_URN  "urn:schemas-upnp-org:service:AVTransport:1"
#define RC_PATH  "/MediaRenderer/RenderingControl/Control"
#define RC_URN   "urn:schemas-upnp-org:service:RenderingControl:1"
#define GRC_PATH "/MediaRenderer/GroupRenderingControl/Control"
#define GRC_URN  "urn:schemas-upnp-org:service:GroupRenderingControl:1"

// Runs an action and discards the response body.
static int simple(const char *ip, const char *path, const char *urn, const char *action, const char *args)
{
	http_response_t r;
	int rc = soap_call(ip, path, urn, action, args, &r, NULL);
	http_response_free(&r);
	return rc;
}

// Runs an action and copies one response element into out.
static int query(const char *ip, const char *path, const char *urn, const char *action,
                 const char *args, const char *tag, char *out, size_t cap)
{
	http_response_t r;
	int rc = soap_call(ip, path, urn, action, args, &r, NULL);
	if (rc == HTTP_OK && !xml_get(r.body, tag, out, cap)) rc = HTTP_ERR_PROTO;
	http_response_free(&r);
	return rc;
}

int sonos_play(const char *ip)     { return simple(ip, AVT_PATH, AVT_URN, "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>"); }
int sonos_pause(const char *ip)    { return simple(ip, AVT_PATH, AVT_URN, "Pause", "<InstanceID>0</InstanceID>"); }
int sonos_next(const char *ip)     { return simple(ip, AVT_PATH, AVT_URN, "Next", "<InstanceID>0</InstanceID>"); }
int sonos_previous(const char *ip) { return simple(ip, AVT_PATH, AVT_URN, "Previous", "<InstanceID>0</InstanceID>"); }

int sonos_seek_time(const char *ip, int s)
{
	char a[128];
	snprintf(a, sizeof a, "<InstanceID>0</InstanceID><Unit>REL_TIME</Unit><Target>%d:%02d:%02d</Target>",
	         s / 3600, (s / 60) % 60, s % 60);
	return simple(ip, AVT_PATH, AVT_URN, "Seek", a);
}

int sonos_seek_track(const char *ip, int n)
{
	char a[128];
	snprintf(a, sizeof a, "<InstanceID>0</InstanceID><Unit>TRACK_NR</Unit><Target>%d</Target>", n);
	return simple(ip, AVT_PATH, AVT_URN, "Seek", a);
}

int sonos_parse_time(const char *s)
{
	int h = 0, m = 0, sec = 0;
	if (sscanf(s, "%d:%d:%d", &h, &m, &sec) != 3) return 0;
	return h * 3600 + m * 60 + sec;
}

int sonos_get_transport_state(const char *ip, transport_state_t *out)
{
	char st[32];
	int rc = query(ip, AVT_PATH, AVT_URN, "GetTransportInfo", "<InstanceID>0</InstanceID>",
	               "CurrentTransportState", st, sizeof st);
	if (rc != HTTP_OK) return rc;
	if (!strcmp(st, "PLAYING")) *out = TS_PLAYING;
	else if (!strcmp(st, "PAUSED_PLAYBACK")) *out = TS_PAUSED;
	else if (!strcmp(st, "STOPPED")) *out = TS_STOPPED;
	else if (!strcmp(st, "TRANSITIONING")) *out = TS_TRANSITIONING;
	else *out = TS_UNKNOWN;
	return HTTP_OK;
}

int sonos_get_position(const char *ip, track_info_t *t)
{
	http_response_t r;
	int rc = soap_call(ip, AVT_PATH, AVT_URN, "GetPositionInfo", "<InstanceID>0</InstanceID>", &r, NULL);
	if (rc != HTTP_OK) { http_response_free(&r); return rc; }

	memset(t, 0, sizeof *t);
	char tmp[32];
	if (xml_get(r.body, "Track", tmp, sizeof tmp)) t->track_num = atoi(tmp);
	if (xml_get(r.body, "RelTime", tmp, sizeof tmp)) t->elapsed_s = sonos_parse_time(tmp);
	if (xml_get(r.body, "TrackDuration", tmp, sizeof tmp)) t->duration_s = sonos_parse_time(tmp);

	// TrackMetaData is a DIDL-Lite document escaped inside the SOAP body.
	const char *c; size_t n;
	if (xml_find(r.body, "TrackMetaData", NULL, &c, &n, NULL) && n > 0) {
		char *didl = (char *)c;
		didl[n] = 0;
		xml_unescape(didl);
		xml_get(didl, "dc:title", t->title, sizeof t->title);
		xml_get(didl, "dc:creator", t->artist, sizeof t->artist);
		xml_get(didl, "upnp:album", t->album, sizeof t->album);
		xml_get(didl, "upnp:albumArtURI", t->art_uri, sizeof t->art_uri);

		// Radio: the "title" is the station; the playing song is streamContent
		// formatted "Artist - Title" (or a free-form show string).
		char stream[160];
		if (xml_get(didl, "r:streamContent", stream, sizeof stream) && stream[0]) {
			t->is_stream = true;
			if (!t->album[0]) str_copy(t->album, t->title, sizeof t->album);
			char *dash = strstr(stream, " - ");
			if (dash) {
				*dash = 0;
				str_copy(t->artist, stream, sizeof t->artist);
				str_copy(t->title, dash + 3, sizeof t->title);
			} else {
				str_copy(t->title, stream, sizeof t->title);
			}
		}
	}
	if (t->duration_s == 0 && t->title[0]) t->is_stream = true;
	http_response_free(&r);
	return HTTP_OK;
}

int sonos_get_play_mode(const char *ip, bool *shuffle, repeat_mode_t *repeat)
{
	char m[32];
	int rc = query(ip, AVT_PATH, AVT_URN, "GetTransportSettings", "<InstanceID>0</InstanceID>",
	               "PlayMode", m, sizeof m);
	if (rc == HTTP_OK) sonos_playmode_from_string(m, shuffle, repeat);
	return rc;
}

int sonos_set_play_mode(const char *ip, bool shuffle, repeat_mode_t repeat)
{
	char a[96];
	snprintf(a, sizeof a, "<InstanceID>0</InstanceID><NewPlayMode>%s</NewPlayMode>",
	         sonos_playmode_to_string(shuffle, repeat));
	return simple(ip, AVT_PATH, AVT_URN, "SetPlayMode", a);
}

void sonos_playmode_from_string(const char *s, bool *shuffle, repeat_mode_t *repeat)
{
	*shuffle = !strncmp(s, "SHUFFLE", 7);
	if (!strcmp(s, "REPEAT_ALL") || !strcmp(s, "SHUFFLE")) *repeat = REPEAT_ALL;
	else if (!strcmp(s, "REPEAT_ONE") || !strcmp(s, "SHUFFLE_REPEAT_ONE")) *repeat = REPEAT_ONE;
	else *repeat = REPEAT_OFF; // NORMAL, SHUFFLE_NOREPEAT
}

const char *sonos_playmode_to_string(bool shuffle, repeat_mode_t repeat)
{
	// Note the asymmetry: plain "SHUFFLE" means shuffle WITH repeat-all.
	if (shuffle) {
		switch (repeat) {
		case REPEAT_ALL: return "SHUFFLE";
		case REPEAT_ONE: return "SHUFFLE_REPEAT_ONE";
		default: return "SHUFFLE_NOREPEAT";
		}
	}
	switch (repeat) {
	case REPEAT_ALL: return "REPEAT_ALL";
	case REPEAT_ONE: return "REPEAT_ONE";
	default: return "NORMAL";
	}
}

int sonos_get_volume(const char *ip, int *vol)
{
	char v[8];
	int rc = query(ip, RC_PATH, RC_URN, "GetVolume", "<InstanceID>0</InstanceID><Channel>Master</Channel>",
	               "CurrentVolume", v, sizeof v);
	if (rc == HTTP_OK) *vol = atoi(v);
	return rc;
}

int sonos_set_volume(const char *ip, int vol)
{
	char a[128];
	if (vol < 0) vol = 0;
	if (vol > 100) vol = 100;
	snprintf(a, sizeof a, "<InstanceID>0</InstanceID><Channel>Master</Channel><DesiredVolume>%d</DesiredVolume>", vol);
	return simple(ip, RC_PATH, RC_URN, "SetVolume", a);
}

int sonos_get_mute(const char *ip, bool *muted)
{
	char v[8];
	int rc = query(ip, RC_PATH, RC_URN, "GetMute", "<InstanceID>0</InstanceID><Channel>Master</Channel>",
	               "CurrentMute", v, sizeof v);
	if (rc == HTTP_OK) *muted = v[0] == '1';
	return rc;
}

int sonos_set_mute(const char *ip, bool muted)
{
	char a[128];
	snprintf(a, sizeof a, "<InstanceID>0</InstanceID><Channel>Master</Channel><DesiredMute>%d</DesiredMute>", muted ? 1 : 0);
	return simple(ip, RC_PATH, RC_URN, "SetMute", a);
}

int sonos_get_group_volume(const char *ip, int *vol)
{
	char v[8];
	// SnapshotGroupVolume makes later relative group changes keep each
	// room's balance; cheap, so do it on every read.
	simple(ip, GRC_PATH, GRC_URN, "SnapshotGroupVolume", "<InstanceID>0</InstanceID>");
	int rc = query(ip, GRC_PATH, GRC_URN, "GetGroupVolume", "<InstanceID>0</InstanceID>",
	               "CurrentVolume", v, sizeof v);
	if (rc == HTTP_OK) *vol = atoi(v);
	return rc;
}

int sonos_set_group_volume(const char *ip, int vol)
{
	char a[96];
	if (vol < 0) vol = 0;
	if (vol > 100) vol = 100;
	snprintf(a, sizeof a, "<InstanceID>0</InstanceID><DesiredVolume>%d</DesiredVolume>", vol);
	return simple(ip, GRC_PATH, GRC_URN, "SetGroupVolume", a);
}

// Builds "<InstanceID>0</InstanceID>" + pre + escaped(uri) + mid + escaped(meta) + post.
static char *build_uri_args(const char *pre, const char *uri, const char *mid, const char *meta, const char *post)
{
	if (!meta) meta = "";
	size_t cap = strlen(pre) + strlen(mid) + strlen(post) + 6 * (strlen(uri) + strlen(meta)) + 64;
	char *a = malloc(cap);
	if (!a) return NULL;
	size_t n = (size_t)snprintf(a, cap, "<InstanceID>0</InstanceID>%s", pre);
	n += xml_escape(uri, a + n, cap - n);
	n += (size_t)snprintf(a + n, cap - n, "%s", mid);
	n += xml_escape(meta, a + n, cap - n);
	snprintf(a + n, cap - n, "%s", post);
	return a;
}

int sonos_set_av_uri(const char *ip, const char *uri, const char *meta)
{
	char *a = build_uri_args("<CurrentURI>", uri, "</CurrentURI><CurrentURIMetaData>", meta, "</CurrentURIMetaData>");
	if (!a) return HTTP_ERR_NOMEM;
	int rc = simple(ip, AVT_PATH, AVT_URN, "SetAVTransportURI", a);
	free(a);
	return rc;
}

int sonos_add_to_queue(const char *ip, const char *uri, const char *meta, int *first_track)
{
	char *a = build_uri_args("<EnqueuedURI>", uri, "</EnqueuedURI><EnqueuedURIMetaData>", meta,
	                         "</EnqueuedURIMetaData><DesiredFirstTrackNumberEnqueued>0</DesiredFirstTrackNumberEnqueued>"
	                         "<EnqueueAsNext>0</EnqueueAsNext>");
	if (!a) return HTTP_ERR_NOMEM;
	char ft[16] = "1";
	int rc = query(ip, AVT_PATH, AVT_URN, "AddURIToQueue", a, "FirstTrackNumberEnqueued", ft, sizeof ft);
	free(a);
	if (first_track) *first_track = atoi(ft);
	return rc;
}

int sonos_clear_queue(const char *ip)
{
	return simple(ip, AVT_PATH, AVT_URN, "RemoveAllTracksFromQueue", "<InstanceID>0</InstanceID>");
}

int sonos_remove_from_queue(const char *ip, int n)
{
	char a[128];
	snprintf(a, sizeof a, "<InstanceID>0</InstanceID><ObjectID>Q:0/%d</ObjectID><UpdateID>0</UpdateID>", n);
	return simple(ip, AVT_PATH, AVT_URN, "RemoveTrackFromQueue", a);
}

int sonos_use_queue(const char *coord_ip, const char *coord_uuid)
{
	char uri[96];
	snprintf(uri, sizeof uri, "x-rincon-queue:%s#0", coord_uuid);
	return sonos_set_av_uri(coord_ip, uri, "");
}

int sonos_join_group(const char *member_ip, const char *coordinator_uuid)
{
	char uri[96];
	snprintf(uri, sizeof uri, "x-rincon:%s", coordinator_uuid);
	return sonos_set_av_uri(member_ip, uri, "");
}

// Hands the group to new_coordinator_uuid; with rejoin=false the old
// coordinator ends up on its own. This is how the Sonos app ungroups a
// group's leader: music keeps playing on the rest of the group and the
// handover is quick, unlike BecomeCoordinatorOfStandaloneGroup on a leader,
// which makes the speaker rebuild the whole group itself (often > 8 s).
int sonos_delegate_coordination(const char *coordinator_ip, const char *new_coordinator_uuid, bool rejoin)
{
	char args[160];
	snprintf(args, sizeof args, "<InstanceID>0</InstanceID><NewCoordinator>%s</NewCoordinator><RejoinGroup>%d</RejoinGroup>",
	         new_coordinator_uuid, rejoin ? 1 : 0);
	return simple(coordinator_ip, AVT_PATH, AVT_URN, "DelegateGroupCoordinationTo", args);
}

int sonos_leave_group(const char *member_ip)
{
	return simple(member_ip, AVT_PATH, AVT_URN, "BecomeCoordinatorOfStandaloneGroup", "<InstanceID>0</InstanceID>");
}
