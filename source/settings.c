#include "settings.h"
#include "xml_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define SETTINGS_PATH SETTINGS_DIR "/settings.ini"
#define HISTORY_PATH SETTINGS_DIR "/history.txt"
#define HISTORY_LINE_MAX (24 * 1024)

settings_t g_settings;

void settings_defaults(settings_t *s)
{
	memset(s, 0, sizeof *s);
	s->volume_step = 2;
	s->show_art = true;
	s->clock_24h = true;
	s->group_volume = true;
	s->theme = 0;
	s->accent = 0;
}

static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t') s++;
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
	return s;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

bool settings_load(settings_t *s)
{
	settings_defaults(s);
	FILE *f = fopen(SETTINGS_PATH, "r");
	if (!f) return false;

	char line[256];
	while (fgets(line, sizeof line, f)) {
		char *l = trim(line);
		if (!*l || *l == '#' || *l == ';') continue;
		char *eq = strchr(l, '=');
		if (!eq) continue;
		*eq = 0;
		char *k = trim(l), *v = trim(eq + 1);

		if (!strcmp(k, "last_room")) str_copy(s->last_room_uuid, v, sizeof s->last_room_uuid);
		else if (!strcmp(k, "preferred_room") && s->preferred_count < MAX_PREFERRED)
			str_copy(s->preferred[s->preferred_count++], v, sizeof s->preferred[0]);
		else if (!strcmp(k, "speaker_ip") && s->manual_count < MAX_MANUAL_IPS)
			str_copy(s->manual_ips[s->manual_count++], v, sizeof s->manual_ips[0]);
		else if (!strcmp(k, "volume_step")) s->volume_step = clampi(atoi(v), 1, 10);
		else if (!strcmp(k, "show_art")) s->show_art = atoi(v) != 0;
		else if (!strcmp(k, "vinyl")) s->vinyl = atoi(v) != 0;
		else if (!strcmp(k, "ozzy")) s->ozzy = atoi(v) != 0;
		else if (!strcmp(k, "party_unlocked")) s->party_unlocked = atoi(v) != 0;
		else if (!strcmp(k, "party")) { s->party = atoi(v); if (s->party < 0 || s->party >= PARTY_COUNT) s->party = PARTY_AUTO; }
		else if (!strcmp(k, "clock_24h")) s->clock_24h = atoi(v) != 0;
		else if (!strcmp(k, "group_volume")) s->group_volume = atoi(v) != 0;
		else if (!strcmp(k, "theme")) {
			// Accept a preset name ("Ocean") or an index.
			s->theme = clampi(atoi(v), 0, THEME_COUNT - 1);
			for (int i = 0; i < THEME_COUNT; i++) if (!strcasecmp(v, THEMES[i].name)) s->theme = i;
		} else if (!strcmp(k, "accent")) {
			s->accent = clampi(atoi(v), 0, ACCENT_COUNT - 1);
			for (int i = 0; i < ACCENT_COUNT; i++) if (!strcasecmp(v, ACCENTS[i].name)) s->accent = i;
		} else if (!strncmp(k, "color.", 6)) {
			// Hand-edited custom colors: color.accent = #FF8800
			for (int i = 0; i < TC_COUNT; i++) {
				if (strcmp(k + 6, THEME_SLOT_NAMES[i])) continue;
				const char *hex = v[0] == '#' ? v + 1 : v;
				if (strlen(hex) == 6) {
					s->color_override[i] = (u32)strtoul(hex, NULL, 16);
					s->color_override_mask |= 1u << i;
				}
			}
		}
	}
	fclose(f);
	return true;
}

bool settings_save(const settings_t *s)
{
	mkdir("sdmc:/3ds", 0777);
	mkdir(SETTINGS_DIR, 0777);
	FILE *f = fopen(SETTINGS_PATH, "w");
	if (!f) return false;

	fprintf(f, "# Sonnis settings. Safe to edit on a PC; unknown keys are ignored.\n\n");
	fprintf(f, "[connection]\n");
	fprintf(f, "last_room = %s\n", s->last_room_uuid);
	for (int i = 0; i < s->preferred_count; i++) fprintf(f, "preferred_room = %s\n", s->preferred[i]);
	fprintf(f, "# Add speaker_ip lines if automatic discovery does not work on your network.\n");
	for (int i = 0; i < s->manual_count; i++) fprintf(f, "speaker_ip = %s\n", s->manual_ips[i]);

	fprintf(f, "\n[behaviour]\n");
	fprintf(f, "volume_step = %d\nshow_art = %d\nclock_24h = %d\ngroup_volume = %d\n",
	        s->volume_step, s->show_art, s->clock_24h, s->group_volume);
	fprintf(f, "vinyl = %d\nozzy = %d\nparty_unlocked = %d\nparty = %d\n", s->vinyl, s->ozzy, s->party_unlocked, s->party);

	fprintf(f, "\n[look]\n");
	fprintf(f, "# Presets:");
	for (int i = 0; i < THEME_COUNT; i++) fprintf(f, " %s", THEMES[i].name);
	fprintf(f, "\ntheme = %s\n", THEMES[s->theme].name);
	fprintf(f, "# Accents:");
	for (int i = 0; i < ACCENT_COUNT; i++) fprintf(f, " %s%s", ACCENTS[i].name, i + 1 < ACCENT_COUNT ? "," : "");
	fprintf(f, "\naccent = %s\n", ACCENTS[s->accent].name);
	fprintf(f, "# Optional per-color overrides (#RRGGBB). Slots:");
	for (int i = 0; i < TC_COUNT; i++) fprintf(f, " %s", THEME_SLOT_NAMES[i]);
	fprintf(f, "\n");
	for (int i = 0; i < TC_COUNT; i++)
		if (s->color_override_mask & (1u << i))
			fprintf(f, "color.%s = #%06lX\n", THEME_SLOT_NAMES[i], (unsigned long)s->color_override[i]);
	fclose(f);
	return true;
}

void settings_apply_theme(const settings_t *s)
{
	theme_apply(s->theme, s->accent, s->color_override_mask, s->color_override);
}

bool settings_is_preferred(const settings_t *s, const char *uuid)
{
	for (int i = 0; i < s->preferred_count; i++)
		if (!strcmp(s->preferred[i], uuid)) return true;
	return false;
}

void settings_toggle_preferred(settings_t *s, const char *uuid)
{
	for (int i = 0; i < s->preferred_count; i++) {
		if (strcmp(s->preferred[i], uuid)) continue;
		memmove(s->preferred[i], s->preferred[i + 1], (size_t)(s->preferred_count - i - 1) * sizeof s->preferred[0]);
		s->preferred_count--;
		return;
	}
	if (s->preferred_count < MAX_PREFERRED) str_copy(s->preferred[s->preferred_count++], uuid, sizeof s->preferred[0]);
}

bool settings_add_manual_ip(settings_t *s, const char *ip)
{
	for (int i = 0; i < s->manual_count; i++) if (!strcmp(s->manual_ips[i], ip)) return true;
	if (s->manual_count >= MAX_MANUAL_IPS) {
		memmove(s->manual_ips[0], s->manual_ips[1], (MAX_MANUAL_IPS - 1) * sizeof s->manual_ips[0]);
		s->manual_count--;
	}
	str_copy(s->manual_ips[s->manual_count++], ip, sizeof s->manual_ips[0]);
	return true;
}

// ---- history: one item per line, fields separated by TAB ----------------

static void flatten(char *s) { for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' '; }

void history_load(lib_list_t *out)
{
	FILE *f = fopen(HISTORY_PATH, "r");
	if (!f) return;
	char *line = malloc(HISTORY_LINE_MAX);
	while (line && fgets(line, HISTORY_LINE_MAX, f)) {
		char *fields[4] = { 0 };
		char *p = line;
		for (int i = 0; i < 4 && p; i++) {
			fields[i] = p;
			p = strchr(p, '\t');
			if (p) *p++ = 0;
		}
		if (!fields[2] || !fields[2][0]) continue;
		trim(fields[3] ? fields[3] : fields[2]);
		lib_item_t *it = lib_list_push(out);
		if (!it) break;
		str_copy(it->title, fields[0], sizeof it->title);
		str_copy(it->subtitle, fields[1], sizeof it->subtitle);
		str_copy(it->uri, fields[2], sizeof it->uri);
		it->meta = fields[3] && fields[3][0] ? strdup(fields[3]) : NULL;
		it->playable = true;
	}
	free(line);
	fclose(f);
	out->total = out->count;
}

void history_add(const lib_item_t *add)
{
	lib_list_t old = { 0 };
	history_load(&old);

	mkdir("sdmc:/3ds", 0777);
	mkdir(SETTINGS_DIR, 0777);
	FILE *f = fopen(HISTORY_PATH, "w");
	if (f) {
		lib_item_t tmp;
		lib_item_copy(&tmp, add);
		flatten(tmp.title); flatten(tmp.subtitle); flatten(tmp.uri);
		if (tmp.meta) flatten(tmp.meta);
		fprintf(f, "%s\t%s\t%s\t%s\n", tmp.title, tmp.subtitle, tmp.uri, tmp.meta ? tmp.meta : "");
		lib_item_free(&tmp);

		int written = 1;
		for (int i = 0; i < old.count && written < MAX_HISTORY; i++) {
			if (!strcmp(old.items[i].uri, add->uri)) continue; // move-to-front dedupe
			fprintf(f, "%s\t%s\t%s\t%s\n", old.items[i].title, old.items[i].subtitle, old.items[i].uri,
			        old.items[i].meta ? old.items[i].meta : "");
			written++;
		}
		fclose(f);
	}
	lib_list_clear(&old);
}
