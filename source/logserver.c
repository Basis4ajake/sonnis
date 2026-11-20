#define _GNU_SOURCE // strcasestr
#include "logserver.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include "netlog.h"
#include <strings.h>
#include "sound.h"

#define LOG_PATH "sdmc:/3ds/sonnis/debug.log"
#define MAX_SEND (128 * 1024) // tail of the file; plenty for debugging

static Thread thread;
static volatile bool running;
static volatile int hits;

static void send_all(int fd, const char *p, size_t n)
{
	while (n > 0) {
		ssize_t s = send(fd, p, n, 0);
		if (s <= 0) return;
		p += s; n -= (size_t)s;
	}
}

#define DUMP_DIR "sdmc:/luma/dumps/arm11"
#define MAX_FILE (4 * 1024 * 1024)

static void respond(int c, const char *type, const char *body, long size)
{
	char hdr[192];
	int hl = snprintf(hdr, sizeof hdr,
	                  "HTTP/1.0 200 OK\r\nContent-Type: %s\r\n"
	                  "Content-Length: %ld\r\nConnection: close\r\n\r\n", type, size);
	send_all(c, hdr, (size_t)hl);
	send_all(c, body, (size_t)size);
}

static void respond_text(int c, const char *msg) { respond(c, "text/plain; charset=utf-8", msg, (long)strlen(msg)); }

// Reads up to max bytes from the END of a file (whole file if smaller).
static char *read_tail(const char *path, long max, long *out_size)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	long start = size > max ? size - max : 0;
	fseek(f, start, SEEK_SET);
	size -= start;
	char *buf = malloc((size_t)size + 1);
	if (buf) size = (long)fread(buf, 1, (size_t)size, f);
	fclose(f);
	*out_size = size;
	return buf;
}

// Only plain file names: no path separators or "..", so nothing outside
// DUMP_DIR can be requested.
static bool safe_name(const char *n)
{
	if (!*n || strstr(n, "..")) return false;
	for (; *n; n++)
		if (!(isalnum((unsigned char)*n) || *n == '_' || *n == '-' || *n == '.')) return false;
	return true;
}

#define INSTALL_PATH "sdmc:/3ds/sonnis/sonnis.3dsx"
#define MAX_UPLOAD (8 * 1024 * 1024)

// Waits up to ms for data and receives it. <= 0 on timeout/close.
static int recv_wait(int c, char *buf, int cap, int ms)
{
	struct pollfd p = { .fd = c, .events = POLLIN };
	if (poll(&p, 1, ms) <= 0) return 0;
	return recv(c, buf, cap, 0);
}

// PUT body -> file. Written to <dest>.part and renamed only when complete,
// so a dropped upload never leaves a half-written app or clip behind.
static void receive_upload(int c, const char *dest, long len, const char *early, int early_n)
{
	char msg[160];
	if (len <= 0 || len > MAX_UPLOAD) { respond_text(c, "need Content-Length (max 8 MB)\n"); return; }
	char part[300];
	snprintf(part, sizeof part, "%s.part", dest);
	FILE *f = fopen(part, "wb");
	if (!f) { respond_text(c, "cannot write to SD card\n"); return; }
	long got = 0;
	if (early_n > 0) { fwrite(early, 1, (size_t)early_n, f); got = early_n; }
	static char buf[16 * 1024];
	// The 3DS socket layer hiccups under sustained transfers (spurious
	// EAGAIN, empty polls), so only give up after 15 s with no progress.
	u64 last_progress = osGetTime();
	int last_rc = 0, last_errno = 0;
	bool write_failed = false;
	while (got < len && running && osGetTime() - last_progress < 15000) {
		int want = len - got < (long)sizeof buf ? (int)(len - got) : (int)sizeof buf;
		struct pollfd p = { .fd = c, .events = POLLIN };
		if (poll(&p, 1, 500) <= 0) continue;
		errno = 0;
		int n = recv(c, buf, want, 0);
		if (n == 0) { last_rc = 0; break; } // sender closed early
		if (n < 0) {
			last_rc = n; last_errno = errno;
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) { svcSleepThread(20 * 1000000LL); continue; }
			break;
		}
		if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { write_failed = true; break; }
		got += n;
		last_progress = osGetTime();
	}
	fclose(f);
	if (got != len) {
		remove(part);
		netlog("upload %s: %ld/%ld bytes, rc %d errno %d%s", dest, got, len, last_rc, last_errno,
		       write_failed ? " (SD write failed)" : "");
		snprintf(msg, sizeof msg, "upload incomplete: %ld of %ld bytes (rc %d, errno %d%s)\n", got, len,
		         last_rc, last_errno, write_failed ? ", SD write failed" : "");
		respond_text(c, msg);
		return;
	}
	remove(dest); // FAT rename won't replace an existing file
	if (rename(part, dest)) { respond_text(c, "rename failed\n"); return; }
	snprintf(msg, sizeof msg, "saved %ld bytes to %s\n", got, dest);
	respond_text(c, msg);
}

static void serve(int c)
{
	// Read until the end of the headers (a PUT body may follow in the same packet).
	char req[1024];
	int used = 0;
	char *body = NULL;
	while (used < (int)sizeof req - 1) {
		int n = recv_wait(c, req + used, (int)sizeof req - 1 - used, 1000);
		if (n <= 0) break;
		used += n;
		req[used] = 0;
		if ((body = strstr(req, "\r\n\r\n"))) { body += 4; break; }
	}
	req[used] = 0;
	// "GET /path HTTP/1.1"
	char method[8] = "GET", path[128] = "/";
	sscanf(req, "%7s %127s", method, path);

	if (!strcmp(method, "PUT")) {
		// PUT /install          -> the app itself (keeps the build on the SD card)
		// PUT /sounds/<name>.wav -> an Ozzy sound bite
		long len = -1;
		char *cl = strcasestr(req, "\r\nContent-Length:");
		if (cl) len = strtol(cl + 17, NULL, 10);
		int early = body ? used - (int)(body - req) : 0;
		char dest[300];
		if (!strcmp(path, "/install")) {
			snprintf(dest, sizeof dest, "%s", INSTALL_PATH);
			receive_upload(c, dest, len, body, early);
		} else if (!strncmp(path, "/sounds/", 8) && safe_name(path + 8) && strlen(path) > 12 &&
		           !strcasecmp(path + strlen(path) - 4, ".wav")) {
			snprintf(dest, sizeof dest, SOUND_DIR "/%s", path + 8);
			receive_upload(c, dest, len, body, early);
		} else {
			respond_text(c, "PUT /install or /sounds/<name>.wav\n");
		}
		goto done;
	}

	if (!strcmp(path, "/dumps")) {
		// Newline-separated list of "name size".
		char list[2048] = "";
		size_t used = 0;
		DIR *d = opendir(DUMP_DIR);
		if (d) {
			struct dirent *e;
			while ((e = readdir(d)) && used < sizeof list - 96) {
				if (e->d_name[0] == '.') continue;
				char full[300];
				struct stat stt;
				snprintf(full, sizeof full, DUMP_DIR "/%s", e->d_name);
				long sz = stat(full, &stt) == 0 ? (long)stt.st_size : -1;
				used += (size_t)snprintf(list + used, sizeof list - used, "%s %ld\n", e->d_name, sz);
			}
			closedir(d);
		}
		respond_text(c, used ? list : "(no crash dumps)\n");
	} else if (!strncmp(path, "/dump/", 6)) {
		const char *name = path + 6;
		if (!safe_name(name)) { respond_text(c, "bad name\n"); goto done; }
		char full[300];
		snprintf(full, sizeof full, DUMP_DIR "/%s", name);
		long size = 0;
		char *body = read_tail(full, MAX_FILE, &size);
		if (!body) { respond_text(c, "not found\n"); goto done; }
		respond(c, "application/octet-stream", body, size);
		free(body);
	} else {
		long size = 0;
		char *body = read_tail(LOG_PATH, MAX_SEND, &size);
		if (body) { respond(c, "text/plain; charset=utf-8", body, size); free(body); }
		else respond_text(c, "(log is empty)\n");
	}
done:
	hits++;
}

static void thread_main(void *arg)
{
	int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (s < 0) { running = false; return; }
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(LOGSERVER_PORT), .sin_addr.s_addr = INADDR_ANY };
	if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 2) < 0) {
		close(s);
		running = false;
		return;
	}
	fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);

	while (running) {
		struct pollfd p = { .fd = s, .events = POLLIN };
		if (poll(&p, 1, 200) <= 0) continue; // wake regularly to notice stop
		int c = accept(s, NULL, NULL);
		if (c < 0) continue;
		fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0) & ~O_NONBLOCK);
		serve(c);
		close(c);
	}
	close(s);
}

void logserver_start(void)
{
	if (running) return;
	running = true;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	thread = threadCreate(thread_main, NULL, 32 * 1024, prio + 1, -1, false);
	if (!thread) running = false;
}

void logserver_stop(void)
{
	if (!thread) return;
	running = false;
	threadJoin(thread, 2ULL * 1000000000ULL);
	threadFree(thread);
	thread = NULL;
}

bool logserver_running(void) { return running; }
int logserver_hits(void) { return hits; }
