#include "sonos_library.h"
#include "sonos_http.h"
#include "sonos_player.h"
#include "xml_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CD_PATH "/MediaServer/ContentDirectory/Control"
#define CD_URN  "urn:schemas-upnp-org:service:ContentDirectory:1"

#define DIDL_HEAD "<DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\" " \
                  "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" " \
                  "xmlns:r=\"urn:schemas-rinconnetworks-com:metadata-1-0/\" " \
                  "xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\">"
#define DIDL_TAIL "</DIDL-Lite>"

lib_item_t *lib_list_push(lib_list_t *l)
{
	if (l->count == l->capacity) {
		int nc = l->capacity ? l->capacity * 2 : 32;
		lib_item_t *n = realloc(l->items, (size_t)nc * sizeof *n);
		if (!n) return NULL;
		l->items = n;
		l->capacity = nc;
	}
	lib_item_t *it = &l->items[l->count++];
	memset(it, 0, sizeof *it);
	return it;
}

void lib_item_free(lib_item_t *it)
{
	free(it->meta);
	it->meta = NULL;
}

void lib_item_copy(lib_item_t *dst, const lib_item_t *src)
{
	*dst = *src;
	dst->meta = src->meta ? strdup(src->meta) : NULL;
}

void lib_list_clear(lib_list_t *l)
{
	for (int i = 0; i < l->count; i++) lib_item_free(&l->items[i]);
	free(l->items);
	memset(l, 0, sizeof *l);
}

static char *dup_n(const char *s, size_t n)
{
	char *d = malloc(n + 1);
	if (d) { memcpy(d, s, n); d[n] = 0; }
	return d;
}

// Parses one <item> or <container> element.
static void parse_entry(const char *open, const char *content, size_t clen, bool container, lib_list_t *out)
{
	lib_item_t *it = lib_list_push(out);
	if (!it) return;
	it->container = container;
	xml_attr(open, "id", it->id, sizeof it->id);

	char *body = dup_n(content, clen);
	if (!body) return;
	xml_get(body, "dc:title", it->title, sizeof it->title);
	if (!xml_get(body, "dc:creator", it->subtitle, sizeof it->subtitle))
		xml_get(body, "upnp:album", it->subtitle, sizeof it->subtitle);
	it->playable = xml_get(body, "res", it->uri, sizeof it->uri) && it->uri[0];

	// Favorites carry the metadata to replay them in <r:resMD>; for anything
	// else the entry itself, wrapped in DIDL-Lite, is the metadata.
	const char *mc; size_t mn;
	if (xml_find(body, "r:resMD", NULL, &mc, &mn, NULL) && mn > 0) {
		it->meta = dup_n(mc, mn);
		if (it->meta) xml_unescape(it->meta);
		it->container = false; // favorites are played, not opened
		// Artist/genre shortcuts from streaming services have no <res>: they
		// only make sense inside the official app, so say so instead of
		// letting A silently do nothing.
		if (!it->playable) {
			it->info_only = true;
			str_copy(it->subtitle, "Shortcut - open in the Sonos app", sizeof it->subtitle);
		}
	} else {
		const char *end_tag = container ? "</container>" : "</item>";
		size_t open_len = (size_t)(content - open);
		size_t need = strlen(DIDL_HEAD) + open_len + clen + strlen(end_tag) + strlen(DIDL_TAIL) + 1;
		it->meta = malloc(need);
		if (it->meta) {
			snprintf(it->meta, need, "%s%.*s%.*s%s%s", DIDL_HEAD, (int)open_len, open, (int)clen, content,
			         end_tag, DIDL_TAIL);
		}
	}
	free(body);
}

int sonos_browse(const char *ip, const char *object_id, int start, int count, lib_list_t *out)
{
	char args[512], esc_id[256];
	if (xml_escape(object_id, esc_id, sizeof esc_id) == (size_t)-1) return HTTP_ERR_PROTO;
	snprintf(args, sizeof args,
	         "<ObjectID>%s</ObjectID><BrowseFlag>BrowseDirectChildren</BrowseFlag>"
	         "<Filter>dc:title,res,dc:creator,upnp:album,upnp:albumArtURI</Filter>"
	         "<StartingIndex>%d</StartingIndex><RequestedCount>%d</RequestedCount>"
	         "<SortCriteria></SortCriteria>", esc_id, start, count);

	http_response_t r;
	int rc = soap_call(ip, CD_PATH, CD_URN, "Browse", args, &r, NULL);
	if (rc != HTTP_OK) { http_response_free(&r); return rc; }

	char tmp[16];
	if (xml_get(r.body, "TotalMatches", tmp, sizeof tmp)) out->total = atoi(tmp);

	const char *c; size_t n;
	if (xml_find(r.body, "Result", NULL, &c, &n, NULL) && n > 0) {
		char *didl = (char *)c;
		didl[n] = 0;
		xml_unescape(didl);
		// Walk the DIDL children in document order, whichever kind comes next.
		const char *p = didl;
		for (;;) {
			const char *io, *ic, *ia, *co, *cc, *ca;
			size_t in, cn;
			bool hi = xml_find(p, "item", &io, &ic, &in, &ia);
			bool hc = xml_find(p, "container", &co, &cc, &cn, &ca);
			if (!hi && !hc) break;
			if (hi && (!hc || io < co)) { parse_entry(io, ic, in, false, out); p = ia; }
			else { parse_entry(co, cc, cn, true, out); p = ca; }
		}
	}
	http_response_free(&r);
	return HTTP_OK;
}

static bool is_stream_uri(const char *u)
{
	static const char *prefixes[] = {
		"x-sonosapi-stream:", "x-sonosapi-radio:", "x-rincon-mp3radio:", "x-sonosapi-hls:",
		"x-sonosapi-hls-static:", "aac:", "hls-radio:", "x-rincon-stream:", "x-sonos-htastream:",
		"x-sonosprog-http:", "http:", "https:",
	};
	for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; i++)
		if (!strncmp(u, prefixes[i], strlen(prefixes[i]))) return true;
	return false;
}

int sonos_play_item(const char *ip, const char *uuid, const lib_item_t *it, const char *parent_id, int index)
{
	int rc;
	if (!it->playable) return HTTP_ERR_PROTO;

	// 1. Something already in the queue: point the transport at the queue
	//    and jump to that track.
	if (parent_id && !strncmp(parent_id, "Q:", 2)) {
		if ((rc = sonos_use_queue(ip, uuid)) != HTTP_OK) return rc;
		if ((rc = sonos_seek_track(ip, index + 1)) != HTTP_OK) return rc;
		return sonos_play(ip);
	}

	// 2. Radio / live streams / line-in: set the transport URI directly.
	// Collections show up three ways: a DIDL <container>, an <item> whose
	// upnp:class is object.container.* (service album favorites), or a
	// cloud-container URI.
	bool meta_is_container = (it->meta && (strstr(it->meta, "<container") || strstr(it->meta, ">object.container"))) ||
	                         !strncmp(it->uri, "x-rincon-cpcontainer:", 21);
	if (is_stream_uri(it->uri) && !meta_is_container) {
		if ((rc = sonos_set_av_uri(ip, it->uri, it->meta)) != HTTP_OK) return rc;
		return sonos_play(ip);
	}

	// 3. Albums, playlists, favorites of collections: replace the queue.
	if (it->container || meta_is_container || !strncmp(it->uri, "file:///jffs/settings/savedqueues", 33)) {
		if ((rc = sonos_clear_queue(ip)) != HTTP_OK) return rc;
		if ((rc = sonos_add_to_queue(ip, it->uri, it->meta, NULL)) != HTTP_OK) return rc;
		if ((rc = sonos_use_queue(ip, uuid)) != HTTP_OK) return rc;
		if ((rc = sonos_seek_track(ip, 1)) != HTTP_OK) return rc;
		return sonos_play(ip);
	}

	// 4. A single track: append to the queue and play it from there, so the
	//    user's existing queue is kept.
	int first = 1;
	if ((rc = sonos_add_to_queue(ip, it->uri, it->meta, &first)) != HTTP_OK) return rc;
	if ((rc = sonos_use_queue(ip, uuid)) != HTTP_OK) return rc;
	if ((rc = sonos_seek_track(ip, first)) != HTTP_OK) return rc;
	return sonos_play(ip);
}

void sonos_list_services_info(lib_list_t *out)
{
	static const char *rows[][2] = {
		{ "Streaming services need cloud sign-in", "Spotify, Apple Music, Amazon, etc." },
		{ "Sonos only exposes them via the cloud API", "which requires OAuth + TLS (not on 2DS)" },
		{ "Workaround: add music to Sonos Favorites", "in the official app, then play it here" },
		{ "Works locally: Favorites, Playlists,", "Queue, Music Library, Radio, Line-In" },
	};
	for (size_t i = 0; i < sizeof rows / sizeof *rows; i++) {
		lib_item_t *it = lib_list_push(out);
		if (!it) return;
		str_copy(it->title, rows[i][0], sizeof it->title);
		str_copy(it->subtitle, rows[i][1], sizeof it->subtitle);
		it->info_only = true;
	}
	out->total = out->count;
}
