#include "controller.h"
#include "diag.h"
#include "netlog.h"
#include "albumart.h"
#include "settings.h"
#include "sonos_http.h"
#include "xml_util.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define QUEUE_LEN 32
#define BROWSE_PAGE 100
#define POLL_MS 1000
#define SLOW_POLL_MS 3000
#define TOPOLOGY_MS 20000
#define REDISCOVER_BACKOFF_MS 6000
#define NET_CHECK_MS 1000

typedef struct {
	cmd_type_t type;
	int a, b;
	char s[160];
} cmd_t;

static ctrl_state_t st;
static LightLock lock;
static LightEvent wake_ev;
static Thread worker;
static volatile bool running;
static volatile bool need_discover;

static cmd_t queue[QUEUE_LEN];
static int q_head, q_len;

static char art_loaded_uri[512];
static int fail_streak;

void ctrl_lock(void) { LightLock_Lock(&lock); }
void ctrl_unlock(void) { LightLock_Unlock(&lock); }
ctrl_state_t *ctrl_state(void) { return &st; }

static void toast_locked(bool err, const char *fmt, va_list ap)
{
	vsnprintf(st.toast, sizeof st.toast, fmt, ap);
	st.toast_until = osGetTime() + (err ? 4000 : 2500);
	st.toast_is_error = err;
}

void ctrl_toast(bool err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	ctrl_lock();
	toast_locked(err, fmt, ap);
	ctrl_unlock();
	va_end(ap);
}

void ctrl_send(cmd_type_t type, int a, int b, const char *s)
{
	ctrl_lock();
	if (q_len == QUEUE_LEN) { // drop the oldest; the user is mashing buttons
		q_head = (q_head + 1) % QUEUE_LEN;
		q_len--;
	}
	cmd_t *c = &queue[(q_head + q_len) % QUEUE_LEN];
	c->type = type; c->a = a; c->b = b;
	str_copy(c->s, s ? s : "", sizeof c->s);
	q_len++;

	// Optimistic UI: flip state immediately so the button feels instant;
	// the next poll corrects it if the speaker disagrees.
	if (type == CMD_PLAY_PAUSE && st.have_status) st.ts = st.ts == TS_PLAYING ? TS_PAUSED : TS_PLAYING;
	if (type == CMD_MUTE_TOGGLE) st.muted = !st.muted;
	if (type == CMD_SHUFFLE_TOGGLE) st.shuffle = !st.shuffle;
	if (type == CMD_REPEAT_CYCLE) st.repeat = (repeat_mode_t)((st.repeat + 1) % 3);
	if (type == CMD_BROWSE && a == 0) {
		lib_list_clear(&st.browse);
		str_copy(st.browse_id, s, sizeof st.browse_id);
		st.browse_loading = true;
		st.browse_err = 0;
		st.browse_gen++;
	}
	if (type == CMD_DIAG) { st.diag_count = 0; st.diag_running = true; }
	if (type == CMD_SELECT_ROOM) {
		str_copy(st.active_uuid, s, sizeof st.active_uuid);
		st.have_status = false;
	}
	ctrl_unlock();
	LightEvent_Signal(&wake_ev);
}

void ctrl_set_volume(int vol)
{
	ctrl_lock();
	st.volume = vol < 0 ? 0 : vol > 100 ? 100 : vol;
	st.vol_dirty = true;
	ctrl_unlock();
	LightEvent_Signal(&wake_ev);
}

void ctrl_on_wake(void)
{
	need_discover = true;
	LightEvent_Signal(&wake_ev);
}

const sonos_speaker_t *ctrl_active_speaker(const ctrl_state_t *s)
{
	for (int i = 0; i < s->speaker_count; i++)
		if (!strcmp(s->speakers[i].uuid, s->active_uuid)) return &s->speakers[i];
	return NULL;
}

int ctrl_estimated_elapsed(const ctrl_state_t *s)
{
	int e = s->track.elapsed_s;
	if (s->ts == TS_PLAYING) e += (int)((osGetTime() - s->track_tick) / 1000);
	if (s->track.duration_s > 0 && e > s->track.duration_s) e = s->track.duration_s;
	return e;
}

// ---- targets -------------------------------------------------------------

typedef struct {
	char room_ip[16], room_uuid[48], coord_ip[16], coord_uuid[48], room_name[64];
	int group_size;
	bool valid;
} target_t;

static target_t get_target(void)
{
	target_t t = { 0 };
	ctrl_lock();
	for (int i = 0; i < st.speaker_count; i++) {
		if (strcmp(st.speakers[i].uuid, st.active_uuid)) continue;
		int c = sonos_find_coordinator(st.speakers, st.speaker_count, i);
		str_copy(t.room_ip, st.speakers[i].ip, sizeof t.room_ip);
		str_copy(t.room_name, st.speakers[i].name, sizeof t.room_name);
		str_copy(t.room_uuid, st.speakers[i].uuid, sizeof t.room_uuid);
		str_copy(t.coord_ip, st.speakers[c].ip, sizeof t.coord_ip);
		str_copy(t.coord_uuid, st.speakers[c].uuid, sizeof t.coord_uuid);
		t.group_size = st.speakers[i].group_size;
		t.valid = true;
		break;
	}
	ctrl_unlock();
	return t;
}

static bool use_group_volume(const target_t *t) { return t->group_size > 1 && g_settings.group_volume; }

static void report(int rc, const char *what, const target_t *t)
{
	if (rc == HTTP_OK) return;
	if (rc == HTTP_ERR_STATUS) {
		// The speaker answered but refused: explain instead of "failed".
		switch (soap_last_upnp_error()) {
		case 701: ctrl_toast(true, "%s is busy - try again in a moment", t->room_name); return;
		case 711:
		case 714:
			ctrl_toast(false, !strcmp(what, "Previous") ? "Already at the first track" :
			                  !strcmp(what, "Next") ? "No next track in the queue" : "Not available here");
			return;
		case 800:
		case 712: ctrl_toast(true, "%s isn't available for this source", what); return;
		case 402: ctrl_toast(true, "%s: the speaker rejected the request", what); return;
		}
	}
	ctrl_toast(true, "%s failed: %s (%s)", what, http_strerror(rc), t->room_name);
}

// ---- discovery ------------------------------------------------------------

// Chooses which room to control after (re)discovery.
static void pick_active_locked(void)
{
	if (ctrl_active_speaker(&st)) return; // still exists
	const char *want[MAX_PREFERRED + 1];
	int n = 0;
	want[n++] = g_settings.last_room_uuid;
	for (int i = 0; i < g_settings.preferred_count; i++) want[n++] = g_settings.preferred[i];
	for (int w = 0; w < n; w++)
		for (int i = 0; i < st.speaker_count; i++)
			if (want[w][0] && !strcmp(st.speakers[i].uuid, want[w]) && !st.speakers[i].invisible) {
				str_copy(st.active_uuid, st.speakers[i].uuid, sizeof st.active_uuid);
				return;
			}
	for (int i = 0; i < st.speaker_count; i++)
		if (!st.speakers[i].invisible && st.speakers[i].is_coordinator) {
			str_copy(st.active_uuid, st.speakers[i].uuid, sizeof st.active_uuid);
			return;
		}
	st.active_uuid[0] = 0;
}

static bool refresh_topology(const char *preferred_ip)
{
	static sonos_speaker_t tmp[MAX_SPEAKERS];
	char ips[MAX_SPEAKERS + MAX_MANUAL_IPS][16];
	int nips = 0;

	if (preferred_ip && preferred_ip[0]) str_copy(ips[nips++], preferred_ip, 16);
	for (int i = 0; i < g_settings.manual_count; i++) str_copy(ips[nips++], g_settings.manual_ips[i], 16);

	int count = -1;
	for (int i = 0; i < nips && count <= 0; i++) count = sonos_fetch_topology(ips[i], tmp, MAX_SPEAKERS);
	if (count <= 0) return false;

	ctrl_lock();
	memcpy(st.speakers, tmp, sizeof(sonos_speaker_t) * (size_t)count);
	st.speaker_count = count;
	pick_active_locked();
	ctrl_unlock();
	return true;
}

static void discover(void)
{
	target_t cur = get_target();
	ctrl_lock();
	st.discovering = true;
	st.scanning = false;
	ctrl_unlock();

	// 1. Fast path (e.g. after waking from sleep): a speaker we already know
	//    still answers, so no need to search at all.
	bool ok = cur.valid && refresh_topology(cur.room_ip);

	// 2. SSDP multicast. Any one speaker knows the whole household.
	char ips[MAX_SPEAKERS][16];
	int n = 0;
	if (!ok) {
		n = sonos_ssdp_search(ips, MAX_SPEAKERS, 2500);
		for (int i = 0; i < n && !ok; i++) ok = refresh_topology(ips[i]);
	}

	// 3. IPs saved in settings (typed in, or learned by a previous scan).
	if (!ok) ok = refresh_topology(NULL);

	// 4. Many mesh / ISP routers filter multicast: probe port 1400 across the
	//    subnet. Remember what we find so next launch takes the fast path.
	if (!ok) {
		ctrl_lock();
		st.scanning = true;
		ctrl_unlock();
		char hits[4][16];
		int h = sonos_subnet_scan((unsigned long)gethostid(), hits, 4);
		for (int i = 0; i < h && !ok; i++) {
			ok = refresh_topology(hits[i]);
			if (ok) {
				ctrl_lock();
				str_copy(st.learned_ip, hits[i], sizeof st.learned_ip);
				ctrl_unlock();
			}
		}
		n += h;
	}

	ctrl_lock();
	st.discovering = false;
	st.scanning = false;
	st.discovered_once = true;
	if (!ok) {
		snprintf(st.toast, sizeof st.toast, n == 0 ? "No Sonos speakers found. See Settings > Add speaker IP."
		                                           : "Found speakers but could not read rooms.");
		st.toast_until = osGetTime() + 5000;
		st.toast_is_error = true;
	} else {
		int visible = 0;
		for (int i = 0; i < st.speaker_count; i++) visible += !st.speakers[i].invisible;
		snprintf(st.toast, sizeof st.toast, "Found %d room%s", visible, visible == 1 ? "" : "s");
		st.toast_until = osGetTime() + 2000;
		st.toast_is_error = false;
	}
	ctrl_unlock();
	fail_streak = 0;
}

// ---- polling --------------------------------------------------------------

static void poll_status(bool slow)
{
	target_t t = get_target();
	if (!t.valid) return;

	transport_state_t ts;
	track_info_t tr;
	int rc = sonos_get_transport_state(t.coord_ip, &ts);
	if (rc == HTTP_OK) rc = sonos_get_position(t.coord_ip, &tr);
	u64 now = osGetTime();

	bool shuffle = false; repeat_mode_t repeat = REPEAT_OFF;
	int vol = -1; bool muted = false;
	int rc_mode = -1, rc_vol = -1, rc_mute = -1;
	if (rc == HTTP_OK && slow) {
		rc_mode = sonos_get_play_mode(t.coord_ip, &shuffle, &repeat);
		rc_vol = use_group_volume(&t) ? sonos_get_group_volume(t.coord_ip, &vol) : sonos_get_volume(t.room_ip, &vol);
		rc_mute = sonos_get_mute(t.room_ip, &muted);
	}

	ctrl_lock();
	bool was_reachable = st.reachable;
	if (strcmp(t.room_uuid, st.active_uuid)) {
		ctrl_unlock(); // room changed while we were polling; discard
		return;
	}
	if (rc == HTTP_OK) {
		st.reachable = true;
		st.have_status = true;
		st.ts = ts;
		st.track = tr;
		st.track_tick = now;
		if (rc_mode == HTTP_OK) { st.shuffle = shuffle; st.repeat = repeat; }
		if (rc_vol == HTTP_OK && !st.vol_dirty) st.volume = vol;
		if (rc_mute == HTTP_OK) st.muted = muted;
		for (int i = 0; i < st.speaker_count; i++)
			if (!strcmp(st.speakers[i].ip, t.coord_ip) || !strcmp(st.speakers[i].ip, t.room_ip)) st.speakers[i].reachable = true;
		fail_streak = 0;
	} else {
		st.reachable = false;
		for (int i = 0; i < st.speaker_count; i++)
			if (!strcmp(st.speakers[i].ip, t.coord_ip)) st.speakers[i].reachable = false;
		if (was_reachable) {
			snprintf(st.toast, sizeof st.toast, "%s unreachable: %s", t.room_name, http_strerror(rc));
			st.toast_until = now + 4000;
			st.toast_is_error = true;
		}
		// Speaker may have changed IP (DHCP) or regrouped: rediscover.
		if (++fail_streak == 3) need_discover = true;
	}
	ctrl_unlock();
}

static void update_art(void)
{
	target_t t = get_target();
	char uri[512];
	ctrl_lock();
	str_copy(uri, st.have_status ? st.track.art_uri : "", sizeof uri);
	ctrl_unlock();
	if (!g_settings.show_art) uri[0] = 0;
	if (!strcmp(uri, art_loaded_uri)) return;
	str_copy(art_loaded_uri, uri, sizeof art_loaded_uri);

	u32 *px = (uri[0] && t.valid) ? albumart_fetch(t.coord_ip, uri) : NULL;
	ctrl_lock();
	free(st.art_pixels);
	st.art_pixels = px;
	st.art_gen++;
	ctrl_unlock();
}

static void push_volume(void)
{
	ctrl_lock();
	bool dirty = st.vol_dirty;
	int vol = st.volume;
	st.vol_dirty = false;
	ctrl_unlock();
	if (!dirty) return;
	target_t t = get_target();
	if (!t.valid) return;
	int rc = use_group_volume(&t) ? sonos_set_group_volume(t.coord_ip, vol) : sonos_set_volume(t.room_ip, vol);
	report(rc, "Volume", &t);
}

// ---- browsing -------------------------------------------------------------

static void do_browse(const char *id, int start)
{
	lib_list_t page = { 0 };
	int rc = HTTP_OK;

	if (!strcmp(id, LIB_ID_HISTORY)) history_load(&page);
	else if (!strcmp(id, LIB_ID_SERVICES)) sonos_list_services_info(&page);
	else {
		target_t t = get_target();
		if (!t.valid) rc = HTTP_ERR_CONNECT;
		else rc = sonos_browse(t.room_ip, id, start, BROWSE_PAGE, &page);
	}

	ctrl_lock();
	if (strcmp(st.browse_id, id)) { // user navigated away meanwhile
		ctrl_unlock();
		lib_list_clear(&page);
		return;
	}
	for (int i = 0; i < page.count; i++) {
		lib_item_t *dst = lib_list_push(&st.browse);
		if (!dst) { lib_item_free(&page.items[i]); continue; }
		*dst = page.items[i]; // move (takes ownership of meta)
	}
	st.browse.total = page.total;
	st.browse_loading = false;
	st.browse_err = rc;
	ctrl_unlock();
	free(page.items); // items moved; free only the array
	if (rc != HTTP_OK) ctrl_toast(true, "Browse failed: %s", http_strerror(rc));
}

static void do_play_item(int index, int gen)
{
	lib_item_t it;
	char parent[160];
	ctrl_lock();
	if (gen != st.browse_gen || index < 0 || index >= st.browse.count) { ctrl_unlock(); return; }
	lib_item_copy(&it, &st.browse.items[index]);
	str_copy(parent, st.browse_id, sizeof parent);
	ctrl_unlock();

	target_t t = get_target();
	if (t.valid && !it.info_only) {
		int rc = sonos_play_item(t.coord_ip, t.coord_uuid, &it, parent, index);
		if (rc == HTTP_OK) {
			ctrl_toast(false, "Playing %s", it.title);
			if (strncmp(parent, "Q:", 2)) history_add(&it);
		} else report(rc, "Play", &t);
	}
	lib_item_free(&it);
}

// ---- diagnostics ----------------------------------------------------------

static void diag_log(const char *line)
{
	ctrl_lock();
	if (st.diag_count < (int)(sizeof st.diag / sizeof st.diag[0]))
		str_copy(st.diag[st.diag_count++], line, sizeof st.diag[0]);
	ctrl_unlock();
	FILE *f = fopen(SETTINGS_DIR "/debug.log", "a");
	if (f) { fprintf(f, "%s\n", line); fclose(f); }
}

static void do_diag(const char *ip)
{
	ctrl_lock();
	st.diag_count = 0;
	st.diag_running = true;
	ctrl_unlock();
	mkdir("sdmc:/3ds", 0777);
	mkdir(SETTINGS_DIR, 0777);
	FILE *f = fopen(SETTINGS_DIR "/debug.log", "a");
	if (f) { fprintf(f, "\n=== diagnostics (Sonnis %s) ===\n", APP_VERSION); fclose(f); }
	diag_run(ip, diag_log);
	ctrl_lock();
	st.diag_running = false;
	ctrl_unlock();
}

// ---- command dispatch -----------------------------------------------------

static void handle(const cmd_t *c)
{
	target_t t = get_target();
	int rc = HTTP_OK;

	switch (c->type) {
	case CMD_DISCOVER: discover(); return;
	case CMD_SELECT_ROOM:
		art_loaded_uri[0] = 1; art_loaded_uri[1] = 0; // force art reload
		return; // state already switched in ctrl_send; next poll fills it
	case CMD_BROWSE: do_browse(c->s, c->a); return;
	case CMD_PLAY_ITEM: do_play_item(c->a, c->b); return;
	case CMD_DIAG: do_diag(c->s); return;
	default: break;
	}
	if (!t.valid) { ctrl_toast(true, "No room selected"); return; }

	switch (c->type) {
	case CMD_PLAY_PAUSE: {
		transport_state_t ts = TS_UNKNOWN;
		sonos_get_transport_state(t.coord_ip, &ts);
		rc = ts == TS_PLAYING ? sonos_pause(t.coord_ip) : sonos_play(t.coord_ip);
		report(rc, "Play/Pause", &t);
		break;
	}
	case CMD_NEXT: report(sonos_next(t.coord_ip), "Next", &t); break;
	case CMD_PREV: report(sonos_previous(t.coord_ip), "Previous", &t); break;
	case CMD_SEEK: report(sonos_seek_time(t.coord_ip, c->a), "Seek", &t); break;
	case CMD_MUTE_TOGGLE: {
		ctrl_lock(); bool m = st.muted; ctrl_unlock(); // already flipped optimistically
		report(sonos_set_mute(t.room_ip, m), "Mute", &t);
		break;
	}
	case CMD_SHUFFLE_TOGGLE:
	case CMD_REPEAT_CYCLE: {
		ctrl_lock(); bool sh = st.shuffle; repeat_mode_t rp = st.repeat; ctrl_unlock();
		rc = sonos_set_play_mode(t.coord_ip, sh, rp);
		// Radio streams reject play modes with UPnP error 712.
		if (rc != HTTP_OK) ctrl_toast(true, "Shuffle/repeat not available for this source");
		break;
	}
	case CMD_QUEUE_REMOVE:
		rc = sonos_remove_from_queue(t.coord_ip, c->a + 1);
		report(rc, "Remove", &t);
		if (rc == HTTP_OK) ctrl_send(CMD_BROWSE, 0, 0, LIB_ID_QUEUE);
		break;
	case CMD_QUEUE_CLEAR:
		rc = sonos_clear_queue(t.coord_ip);
		report(rc, "Clear queue", &t);
		if (rc == HTTP_OK) ctrl_send(CMD_BROWSE, 0, 0, LIB_ID_QUEUE);
		break;
	case CMD_JOIN:
	case CMD_LEAVE: {
		char member_ip[16] = "", member_name[64] = "";
		ctrl_lock();
		for (int i = 0; i < st.speaker_count; i++)
			if (!strcmp(st.speakers[i].uuid, c->s)) {
				str_copy(member_ip, st.speakers[i].ip, sizeof member_ip);
				str_copy(member_name, st.speakers[i].name, sizeof member_name);
			}
		ctrl_unlock();
		if (!member_ip[0]) break;
		if (c->type == CMD_JOIN) {
			// A room can't join a group it's already in (Sonos answers 402).
			bool already = false;
			ctrl_lock();
			for (int i = 0; i < st.speaker_count; i++)
				if (!strcmp(st.speakers[i].uuid, c->s) && !strcmp(st.speakers[i].coordinator_uuid, t.coord_uuid))
					already = true;
			ctrl_unlock();
			if (already) { ctrl_toast(false, "%s is already in this group", member_name); break; }
		}
		// A group leader leaving: hand the group to another member first.
		char heir_uuid[48] = "";
		if (c->type == CMD_LEAVE) {
			ctrl_lock();
			bool leader = false;
			for (int i = 0; i < st.speaker_count; i++)
				if (!strcmp(st.speakers[i].uuid, c->s)) leader = st.speakers[i].is_coordinator && st.speakers[i].group_size > 1;
			for (int i = 0; leader && i < st.speaker_count; i++) {
				const sonos_speaker_t *m = &st.speakers[i];
				if (strcmp(m->coordinator_uuid, c->s) || !strcmp(m->uuid, c->s) || m->invisible) continue;
				// Prefer a member that answered recently.
				if (!heir_uuid[0] || m->reachable) str_copy(heir_uuid, m->uuid, sizeof heir_uuid);
				if (m->reachable) break;
			}
			ctrl_unlock();
		}
		if (c->type == CMD_JOIN) rc = sonos_join_group(member_ip, t.coord_uuid);
		else if (heir_uuid[0]) {
			rc = sonos_delegate_coordination(member_ip, heir_uuid, false);
			netlog("leave: %s delegated to %s -> %d", member_name, heir_uuid, rc);
			if (rc == HTTP_ERR_STATUS) rc = sonos_leave_group(member_ip); // older firmware
		} else rc = sonos_leave_group(member_ip);
		if (rc == HTTP_OK) ctrl_toast(false, c->type == CMD_JOIN ? "%s joined %s" : "%s is now on its own",
		                              member_name, t.room_name);
		else report(rc, c->type == CMD_JOIN ? "Group" : "Ungroup", &t);
		svcSleepThread(600 * 1000000LL); // let the household settle
		refresh_topology(t.room_ip);
		break;
	}
	default: break;
	}
}

// ---- worker loop ------------------------------------------------------------

static void worker_main(void *arg)
{
	u64 next_poll = 0, next_slow = 0, next_topo = 0, next_net = 0, next_discover = 0;
	net_status_t last_net = NET_DOWN;

	while (running) {
		LightEvent_WaitTimeout(&wake_ev, 200 * 1000000LL);
		if (!running) break;
		u64 now = osGetTime();

		if (now >= next_net) {
			net_status_t ns = network_status();
			char ip[16] = "0.0.0.0";
			if (ns == NET_UP) network_local_ip(ip);
			ctrl_lock();
			st.net = ns;
			str_copy(st.local_ip, ip, sizeof st.local_ip);
			ctrl_unlock();
			if (ns == NET_UP && last_net == NET_DOWN) need_discover = true; // (re)connected
			if (ns == NET_DOWN && last_net == NET_UP) ctrl_toast(true, "Wi-Fi disconnected - waiting...");
			last_net = ns;
			next_net = now + NET_CHECK_MS;
		}
		if (last_net != NET_UP) {
			// Commands can't succeed offline; drop them rather than replay stale presses later.
			ctrl_lock(); q_len = 0; st.browse_loading = false; ctrl_unlock();
			continue;
		}

		if (need_discover && now >= next_discover) {
			need_discover = false;
			discover();
			next_discover = osGetTime() + REDISCOVER_BACKOFF_MS;
			next_topo = osGetTime() + TOPOLOGY_MS;
			next_poll = next_slow = 0;
		}

		// Drain commands.
		for (;;) {
			cmd_t c;
			ctrl_lock();
			bool have = q_len > 0;
			if (have) { c = queue[q_head]; q_head = (q_head + 1) % QUEUE_LEN; q_len--; }
			ctrl_unlock();
			if (!have) break;
			handle(&c);
			if (c.type != CMD_BROWSE) next_poll = 0; // reflect the change quickly
			if (c.type == CMD_SELECT_ROOM) next_slow = 0;
		}
		push_volume();

		now = osGetTime();
		if (now >= next_topo) {
			target_t t = get_target();
			refresh_topology(t.valid ? t.room_ip : NULL);
			next_topo = now + TOPOLOGY_MS;
		}
		if (now >= next_poll) {
			bool slow = now >= next_slow;
			poll_status(slow);
			update_art();
			next_poll = osGetTime() + POLL_MS;
			if (slow) next_slow = osGetTime() + SLOW_POLL_MS;
		}
	}
}

void ctrl_start(void)
{
	memset(&st, 0, sizeof st);
	LightLock_Init(&lock);
	LightEvent_Init(&wake_ev, RESET_ONESHOT);
	running = true;
	need_discover = true;

	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// Lower priority than the UI (higher number) so rendering always wins;
	// the worker spends most of its time blocked in socket calls anyway.
	worker = threadCreate(worker_main, NULL, 64 * 1024, prio + 1, -1, false);
}

bool ctrl_stop(void)
{
	running = false;
	net_abort_all();            // make every in-flight network wait bail out
	LightEvent_Signal(&wake_ev);
	bool stopped = true;
	if (worker) {
		// With the abort the worker exits in well under a second; the long
		// limit only guards against something unforeseen.
		stopped = R_SUCCEEDED(threadJoin(worker, 5ULL * 1000000000ULL));
		if (!stopped) return false; // still running: leave its memory alone
		threadFree(worker);
		worker = NULL;
	}
	lib_list_clear(&st.browse);
	free(st.art_pixels);
	st.art_pixels = NULL;
	return stopped;
}
