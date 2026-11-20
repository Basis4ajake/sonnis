#include "ozzy.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

bool g_ozzy;

typedef struct { const char *from, *to; } vox_t;

// Original lines in the spirit of a certain Birmingham metal legend:
// f-bombs first (his signature), plus Brummie/British pub swearing.
// Diagnostics and the network log are left alone: those need to stay exact.
static const vox_t TABLE[] = {
	// top screen
	{ "NOW PLAYING", "NOW FUCKING ROCKING" },
	{ "PAUSED", "HAVING A FUCKING FAG BREAK" },
	{ "STOPPED", "SILENCE. FUCKING HELL." },
	{ "LOADING", "SHAAARON! IT'S LOADING!" },
	{ "Nothing playing", "Nothing's on, you daft twat" },
	{ "Pick something in Browse", "Pick some fucking music in Riffs, wanker" },
	{ "Not connected to Wi-Fi", "Where's the fucking Wi-Fi, Sharon?!" },
	{ "Turn on wireless and connect in System Settings.", "Flip the bloody wireless on in System Settings, you tosspot." },
	{ "Sonnis will reconnect automatically.", "I'll crawl back on me own. Always fucking do." },
	{ "No speakers found", "No fucking speakers! Bollocks!" },
	{ "Check the 2DS is on the same Wi-Fi as your Sonos.", "Is this shite even on the same Wi-Fi as the amps?" },
	{ "START > Add speaker IP, or Refresh discovery.", "START > Add speaker IP, or try again, for fuck's sake." },
	{ "Pick a room", "Pick a fucking room" },
	{ "Tap Rooms on the touch screen", "Poke Rooms on the bottom screen, you plonker" },
	{ "Looking for speakers on your network", "Hunting down the bastard amplifiers" },
	{ "Discovery is blocked here; checking each address", "Some cunt blocked discovery; kicking every fucking door" },
	{ "Searching for Sonos*", "Where the fuck are the speakers*" },
	{ "Scanning network*", "Kicking down fucking doors*" },
	{ "Connecting to *", "Oi! Getting through to " },
	{ "SHUFFLE", "MIX IT UP" },
	{ "REPEAT ALL", "AGAIN! FUCKING AGAIN!" },
	{ "REPEAT ONE", "THIS ONE AGAIN" },
	{ "REPEAT OFF", "NO FUCKING ENCORE" },
	{ "MUTED", "SHUT IT" },

	// bottom: now playing
	{ "Browse", "Riffs" },
	{ "Favorites", "Bangers" },
	{ "Settings", "Knobs" },
	{ "Shuffle", "Mix it up" },
	{ "Repeat", "Encore" },
	{ "Repeat all", "Encore all" },
	{ "Repeat one", "Just this one" },

	// browse / rooms
	{ "Sonos Favorites", "The Fucking Bangers" },
	{ "Recently Played", "Shite You Just Played" },
	{ "Queue", "The Fucking Setlist" },
	{ "Sonos Playlists", "Bollocking Mixtapes" },
	{ "Music library", "The Record Collection" },
	{ "Radio Stations", "The Bloody Wireless" },
	{ "Music Services", "Corporate Wankers (cloud)" },
	{ "Nothing here yet", "Fuck all in here" },
	{ "Couldn't load this list", "Fucking thing won't load" },
	{ "Loading*", "Hang on, for fuck's sake*" },
	{ "Searching*", "Looking*" },
	{ "No rooms found", "No fucking rooms, you twat" },
	{ "Unreachable", "Passed out pissed" },
	{ "Pin room", "Pin it" },
	{ "Refresh", "Again!" },

	// settings
	{ "Theme", "Look" },
	{ "Accent color", "War paint" },
	{ "Album artwork", "Album covers" },
	{ "Clock", "What fucking time is it" },
	{ "Quit Sonnis", "Fuck off out of it" },
	{ "Refresh discovery", "Find the fucking speakers again" },
	{ "Add speaker IP...", "Tell me where the bastard speaker is..." },
	{ "Clear saved speaker IPs", "Bin the fucking speaker IPs" },
	{ "Cleared saved IPs", "Binned 'em. Bollocks to 'em." },

	// toasts (from the worker)
	{ "Playing *", "Cranking the fuck out of " },
	{ "No next track in the queue", "That's the last fucking song, you wanker!" },
	{ "Already at the first track", "Can't go back any further, you tosspot" },
	{ "No room selected", "Pick a room first, you daft cunt" },
	{ "Wi-Fi disconnected - waiting...", "Lost the fucking Wi-Fi. Waiting..." },
	{ "Shuffle/repeat not available for this source", "Can't fucking shuffle the radio, you twat" },
	{ "*is busy - try again in a moment", "is fucking busy. Give it a sec, Sharon." },
	{ "*is already in this group", "is already in the fucking band" },
	{ "*is now on its own", "went solo. Ungrateful bastard." },
	{ "* failed: speaker timed out*", " - the speaker fucked off and stopped answering" },
	{ "* failed: speaker unreachable*", " - the speaker's passed out cold. Wanker." },
	{ "* failed: connection dropped*", " - some twat dropped the connection" },
	{ "* failed: speaker rejected request*", " - speaker told me to fuck off" },
	{ "*: the speaker rejected the request", ": the speaker told me to fuck off" },
	{ "*isn't available for this source", "won't fucking work on this, mate" },
	{ "These go to eleven.", "ELEVEN! LOUDER, YOU FUCKING WANKERS!" },
	{ "The bears are dancing", "Who let the fucking bears in?!" },
	{ "The bears went home", "Bears fucked off home" },
	{ "Party mode", "Fucking Party Mode" },
	{ "Press again to quit", "Again if you really want me to fuck off" },
	{ "Party mode unlocked - it's in Settings now", "PARTY MODE, YOU BASTARDS! It's in the Knobs" },
	{ "Forced on for every song", "Every fucking song, mate" },
	{ "Circle pad scratches the record", "Circle pad scratches the fucking record" },
};

#define MAX_ROOMS 32
static char rooms[MAX_ROOMS][64];
static int room_n;

void vox_set_rooms(const char *const *names, int n)
{
	room_n = 0;
	for (int i = 0; i < n && room_n < MAX_ROOMS; i++) {
		if (!names[i][0]) continue;
		bool dup = false;
		for (int j = 0; j < room_n && !dup; j++) dup = !strcmp(rooms[j], names[i]);
		if (dup) continue;
		snprintf(rooms[room_n++], sizeof rooms[0], "%s", names[i]);
	}
	// Longest first, so "Back Office" wins over "Office".
	for (int i = 1; i < room_n; i++)
		for (int j = i; j > 0 && strlen(rooms[j]) > strlen(rooms[j - 1]); j--) {
			char t[64];
			memcpy(t, rooms[j], 64); memcpy(rooms[j], rooms[j - 1], 64); memcpy(rooms[j - 1], t, 64);
		}
}

// Same room always gets the same word.
static const char *room_prefix(const char *name)
{
	unsigned h = 5381;
	for (; *name; name++) h = h * 33 + (unsigned char)*name;
	return (h >> 3) & 1 ? "Fucking " : "Bloody ";
}

static const char *prefix_rooms(const char *s, char *out, size_t cap)
{
	if (!room_n) return s;
	size_t o = 0;
	bool any = false;
	for (const char *p = s; *p && o + 1 < cap;) {
		int hit = -1;
		size_t hl = 0;
		if (p == s || !isalnum((unsigned char)p[-1]))
			for (int i = 0; i < room_n; i++) {
				size_t l = strlen(rooms[i]);
				if (!strncmp(p, rooms[i], l) && !isalnum((unsigned char)p[l])) { hit = i; hl = l; break; }
			}
		if (hit < 0) { out[o++] = *p++; continue; }
		int w = snprintf(out + o, cap - o, "%s%.*s", room_prefix(rooms[hit]), (int)hl, p);
		o = w < 0 ? o : o + (size_t)w >= cap ? cap - 1 : o + (size_t)w;
		p += hl;
		any = true;
	}
	out[o] = 0;
	return any ? out : s;
}

static const char *table_pass(const char *s, char *buf, size_t cap);

const char *vox(const char *s, char *buf, size_t cap)
{
	if (!g_ozzy || !s || !*s) return s;
	// Table pass writes into buf (or returns s / a constant); the room pass
	// writes into tmp, which is copied back so tmp is never returned.
	char tmp[256];
	const char *t = table_pass(s, buf, cap);
	if (prefix_rooms(t, tmp, sizeof tmp) == t) return t;
	snprintf(buf, cap, "%s", tmp);
	return buf;
}

static const char *table_pass(const char *s, char *buf, size_t cap)
{
	size_t n = strlen(s);
	for (size_t i = 0; i < sizeof TABLE / sizeof *TABLE; i++) {
		const char *k = TABLE[i].from;
		size_t kl = strlen(k);
		if (k[0] == '*') {
			// "*middle*" = contains; "*suffix" = ends with. Keep the start of s.
			bool contains = k[kl - 1] == '*';
			size_t ml = kl - 1 - contains;
			const char *hit = NULL;
			if (contains) {
				char key[64];
				snprintf(key, sizeof key, "%.*s", (int)ml, k + 1);
				hit = strstr(s, key);
			} else if (n >= ml && !strcmp(s + n - ml, k + 1)) {
				hit = s + n - ml;
			}
			if (!hit) continue;
			snprintf(buf, cap, "%.*s%s", (int)(hit - s), s, TABLE[i].to);
			return buf;
		}
		if (k[kl - 1] == '*') {
			if (strncmp(s, k, kl - 1)) continue;
			// "Loading*" -> "Hang on*": strip the '*' from to, keep the rest of s.
			const char *to = TABLE[i].to;
			size_t tl = strlen(to);
			if (tl && to[tl - 1] == '*') tl--;
			snprintf(buf, cap, "%.*s%s", (int)tl, to, s + kl - 1);
			return buf;
		}
		if (!strcmp(s, k)) return TABLE[i].to;
	}
	return s;
}
