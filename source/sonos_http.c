#define _GNU_SOURCE // strcasestr
#include "sonos_http.h"
#include "xml_util.h"
#include "netlog.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include <limits.h>

#include <3ds.h> // osGetTime

// Generous: a 2DS on Wi-Fi sees 50-270 ms RTTs, and a lost SYN costs a ~1 s
// retransmit. Power-saving speakers can also be slow to answer the first SYN.
// The 2DS's first SYN to a host whose ARP entry has expired is dropped, and
// TCP only retransmits after 3 s (seen in on-device logs: connects completing
// at 3014-3040 ms). Leave room for that retransmit.
#define CONNECT_TIMEOUT_MS 7000

// ---- ARP warm-up ------------------------------------------------------------
// Small stacks often discard the packet that triggers ARP resolution. Sending
// a throwaway UDP datagram first lets ARP resolve on *that* packet, so the
// real SYN goes straight out instead of waiting for TCP's 3 s retransmit.
#define WARM_SLOTS 32
#define WARM_TTL_MS 60000
static struct { u32 ip; u64 when; } warm[WARM_SLOTS];

static void arp_warmup(const struct sockaddr_in *sa)
{
	u64 now = osGetTime();
	int slot = 0;
	for (int i = 0; i < WARM_SLOTS; i++) {
		if (warm[i].ip == sa->sin_addr.s_addr) {
			if (now - warm[i].when < WARM_TTL_MS) { warm[i].when = now; return; }
			slot = i;
			break;
		}
		if (warm[i].when < warm[slot].when) slot = i; // LRU victim
	}
	int u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (u >= 0) {
		struct sockaddr_in d = *sa;
		d.sin_port = htons(9); // "discard" port; content irrelevant
		sendto(u, "", 1, 0, (struct sockaddr *)&d, sizeof d);
		close(u);
		svcSleepThread(60 * 1000000LL); // a LAN ARP round trip is a few ms
	}
	warm[slot].ip = sa->sin_addr.s_addr;
	warm[slot].when = now;
}

static void arp_forget(const struct sockaddr_in *sa)
{
	for (int i = 0; i < WARM_SLOTS; i++)
		if (warm[i].ip == sa->sin_addr.s_addr) warm[i].when = 0;
}

static volatile bool aborting;
void net_abort_all(void) { aborting = true; }
bool net_aborting(void) { return aborting; }

const char *http_strerror(int err)
{
	switch (err) {
	case HTTP_OK: return "OK";
	case HTTP_ERR_SOCKET: return "socket error";
	case HTTP_ERR_CONNECT: return "speaker unreachable";
	case HTTP_ERR_TIMEOUT: return "speaker timed out";
	case HTTP_ERR_IO: return "connection dropped";
	case HTTP_ERR_PROTO: return "bad response";
	case HTTP_ERR_NOMEM: return "out of memory";
	case HTTP_ERR_TOOBIG: return "response too large";
	case HTTP_ERR_STATUS: return "speaker rejected request";
	case HTTP_ERR_ABORTED: return "cancelled";
	default: return "unknown error";
	}
}

static int last_connect_errno;
static bool last_connect_timed_out;

int sock_connect_status(int fd, const struct sockaddr_in *sa)
{
	// Ask the stack directly by calling connect() again, instead of trusting
	// poll() flags: libctru's own poll.h marks POLLHUP/POLLERR as uncertain,
	// and Linux and the 3DS disagree on which bit means what.
	errno = 0;
	int rc = connect(fd, (const struct sockaddr *)sa, sizeof *sa);
	if (rc == 0 || errno == EISCONN) return 1;
	if (errno == EINPROGRESS || errno == EALREADY || errno == EWOULDBLOCK || errno == EAGAIN) return 0;
	last_connect_errno = errno;
	return -1;
}

static int connect_once(const char *ip, int port)
{
	int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (fd < 0) return HTTP_ERR_SOCKET;

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	if (inet_aton(ip, &sa.sin_addr) == 0) { close(fd); return HTTP_ERR_CONNECT; }

	arp_warmup(&sa);

	// Non-blocking connect + poll gives us a real connect timeout; a blocking
	// connect to a powered-off speaker would otherwise hang for ~20 s.
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	last_connect_errno = 0;
	u64 start = osGetTime();
	int st = sock_connect_status(fd, &sa);
	int first_errno = errno, polls = 0;
	u64 deadline = start + CONNECT_TIMEOUT_MS;
	while (st == 0 && !aborting) {
		int left = (int)(deadline - osGetTime());
		if (left <= 0) break;
		struct pollfd pfd = { .fd = fd, .events = POLLOUT };
		if (poll(&pfd, 1, left > 250 ? 250 : left) < 0) break;
		polls++;
		st = sock_connect_status(fd, &sa);
	}
	if (st != 1) {
		netlog("conn %s %s e1=%d e=%d %dms p=%d", ip, st == 0 ? "TIMEOUT" : "FAIL", first_errno,
		       last_connect_errno, (int)(osGetTime() - start), polls);
		close(fd);
		arp_forget(&sa); // re-warm on the retry
		last_connect_timed_out = st == 0;
		return HTTP_ERR_CONNECT;
	}
	fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
	return fd;
}

static int connect_timeout(const char *ip, int port)
{
	// One retry: on Wi-Fi a single dropped packet shouldn't make a speaker
	// look unreachable.
	int fd = connect_once(ip, port);
	// Retry only fast failures. A full 7 s timeout already covered TCP's
	// retransmit, so the speaker is most likely off; don't double the wait.
	if (fd == HTTP_ERR_CONNECT && !last_connect_timed_out && !aborting) {
		// Pause before retrying: a speaker waking from Wi-Fi power-save
		// ignores everything for a moment, and an instant retry lands in
		// the same stall.
		svcSleepThread(300 * 1000000LL);
		fd = connect_once(ip, port);
	}
	return fd;
}

static int send_all(int fd, const char *buf, size_t len)
{
	while (len > 0) {
		ssize_t n = send(fd, buf, len, 0);
		if (n <= 0) return HTTP_ERR_IO;
		buf += n; len -= (size_t)n;
	}
	return HTTP_OK;
}

// Decodes a chunked body in place. Returns the decoded length or -1.
static long dechunk(char *body, size_t len)
{
	char *r = body, *w = body, *end = body + len;
	while (r < end) {
		char *eol = strstr(r, "\r\n");
		if (!eol) return -1;
		unsigned long n = strtoul(r, NULL, 16);
		r = eol + 2;
		if (n == 0) break;
		if (r + n > end) n = (unsigned long)(end - r); // tolerate truncation
		memmove(w, r, n);
		w += n; r += n;
		if (r + 2 <= end && r[0] == '\r') r += 2;
	}
	*w = 0;
	return (long)(w - body);
}

int http_request(const char *ip, int port, const char *method, const char *path,
                 const char *extra_headers, const char *body, size_t body_len,
                 http_response_t *resp, size_t max_bytes, int timeout_ms)
{
	memset(resp, 0, sizeof *resp);
	if (aborting) return HTTP_ERR_ABORTED;
	int fd = connect_timeout(ip, port);
	if (fd < 0) return aborting ? HTTP_ERR_ABORTED : fd;

	char hdr[768];
	int hl = snprintf(hdr, sizeof hdr,
	                  "%s %s HTTP/1.1\r\n"
	                  "Host: %s:%d\r\n"
	                  "Connection: close\r\n"
	                  "User-Agent: Sonnis/1.0 (Nintendo 3DS)\r\n"
	                  "Content-Length: %u\r\n"
	                  "%s\r\n",
	                  method, path, ip, port, (unsigned)body_len, extra_headers ? extra_headers : "");
	if (hl <= 0 || hl >= (int)sizeof hdr) { close(fd); return HTTP_ERR_PROTO; }

	int rc = send_all(fd, hdr, (size_t)hl);
	if (rc == HTTP_OK && body_len) rc = send_all(fd, body, body_len);
	if (rc != HTTP_OK) {
		netlog("%s SEND-ERR %s e=%d", ip, path, errno);
		close(fd);
		return rc;
	}

	size_t cap = 8192, len = 0;
	char *buf = malloc(cap);
	if (!buf) { close(fd); return HTTP_ERR_NOMEM; }

	u64 t_start = osGetTime(), t_last = t_start;
	long content_len = -1;   // from headers, once seen
	size_t header_len = 0;
	int recv_errno = 0;
	for (;;) {
		// Done as soon as the declared body has arrived; don't depend on the
		// server's close (a reset instead of a FIN would look like an error).
		if (content_len >= 0 && len >= header_len + (size_t)content_len) { rc = HTTP_OK; break; }
		// Wait in short slices so a shutdown is noticed promptly.
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int pr = 0;
		while (!aborting) {
			int left = timeout_ms - (int)(osGetTime() - t_last);
			if (left <= 0) break;
			pfd.revents = 0;
			pr = poll(&pfd, 1, left > 250 ? 250 : left);
			if (pr != 0) break;
		}
		if (aborting) { rc = HTTP_ERR_ABORTED; break; }
		if (pr == 0) { rc = HTTP_ERR_TIMEOUT; break; }
		if (pr < 0) { rc = HTTP_ERR_IO; recv_errno = errno; break; }
		if (len + 4096 + 1 > cap) {
			if (cap >= max_bytes) { rc = HTTP_ERR_TOOBIG; break; }
			size_t ncap = cap * 2 > max_bytes ? max_bytes : cap * 2;
			char *nb = realloc(buf, ncap);
			if (!nb) { rc = HTTP_ERR_NOMEM; break; }
			buf = nb; cap = ncap;
		}
		ssize_t n = recv(fd, buf + len, cap - len - 1, 0);
		if (n < 0) { rc = HTTP_ERR_IO; recv_errno = errno; break; }
		if (n == 0) { rc = HTTP_OK; break; } // server closed: response complete
		len += (size_t)n;
		t_last = osGetTime(); // timeout is per gap between packets
		if (content_len < 0) {
			buf[len] = 0;
			char *he = strstr(buf, "\r\n\r\n");
			if (he) {
				header_len = (size_t)(he + 4 - buf);
				*he = 0;
				const char *cl = strcasestr(buf, "\r\nContent-Length:");
				content_len = cl ? atol(cl + 17) : -2; // -2: no length, read to close
				*he = '\r';
				if (content_len == -2) content_len = LONG_MAX / 2;
			}
		}
	}
	close(fd);
	if (rc == HTTP_ERR_IO && content_len >= 0 && content_len < LONG_MAX / 2 &&
	    len >= header_len + (size_t)content_len)
		rc = HTTP_OK; // got everything before the reset
	if (rc == HTTP_ERR_ABORTED) { free(buf); return rc; }
	if (rc != HTTP_OK) {
		netlog("%s %s %s after %dms got=%u e=%d", ip, rc == HTTP_ERR_TIMEOUT ? "TIMEOUT" : "RECV-ERR",
		       path, (int)(osGetTime() - t_start), (unsigned)len, recv_errno);
		free(buf);
		return rc;
	}
	buf[len] = 0;

	// Status line + headers
	char *hend = strstr(buf, "\r\n\r\n");
	if (!hend || strncmp(buf, "HTTP/1.", 7) != 0) { free(buf); return HTTP_ERR_PROTO; }
	resp->status = atoi(buf + 9);
	*hend = 0;
	bool chunked = false;
	for (char *h = strstr(buf, "\r\n"); h; h = strstr(h + 2, "\r\n")) {
		if (!strncasecmp(h + 2, "Transfer-Encoding:", 18) && strstr(h + 2, "chunked")) chunked = true;
	}

	char *bstart = hend + 4;
	size_t blen = len - (size_t)(bstart - buf);
	memmove(buf, bstart, blen);
	buf[blen] = 0;
	if (chunked) {
		long d = dechunk(buf, blen);
		if (d < 0) { free(buf); return HTTP_ERR_PROTO; }
		blen = (size_t)d;
	}
	resp->body = buf;
	resp->len = blen;
	return (resp->status >= 200 && resp->status < 300) ? HTTP_OK : HTTP_ERR_STATUS;
}

void http_response_free(http_response_t *r)
{
	free(r->body);
	r->body = NULL;
	r->len = 0;
}

bool http_parse_url(const char *url, char *host, size_t host_cap, int *port,
                    char *path, size_t path_cap)
{
	if (strncmp(url, "http://", 7) != 0) return false;
	const char *h = url + 7;
	const char *slash = strchr(h, '/');
	const char *colon = strchr(h, ':');
	if (colon && slash && colon > slash) colon = NULL;
	const char *hend = colon ? colon : (slash ? slash : h + strlen(h));
	str_copy_n(host, h, (size_t)(hend - h), host_cap);
	*port = colon ? atoi(colon + 1) : 80;
	str_copy(path, slash ? slash : "/", path_cap);
	return host[0] != 0;
}

// Safe to repeat if we can't tell whether the speaker already ran it: these
// set a state rather than step it. Next/Previous/queue edits are not.
static int last_upnp_error;

static bool idempotent(const char *action)
{
	// Repeating these after a lost reply would act twice (or, for the group
	// changes, fail because the first attempt already worked).
	static const char *unsafe[] = { "Next", "Previous", "AddURIToQueue", "RemoveTrackFromQueue",
	                                "DelegateGroupCoordinationTo", "BecomeCoordinatorOfStandaloneGroup" };
	for (size_t i = 0; i < sizeof unsafe / sizeof *unsafe; i++)
		if (!strcmp(action, unsafe[i])) return false;
	return true;
}

static int soap_once(const char *ip, const char *control_path, const char *service_type,
                     const char *action, const char *args_xml, http_response_t *resp,
                     int *upnp_error)
{
	if (upnp_error) *upnp_error = 0;
	const char *fmt =
	    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
	    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
	    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
	    "<s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>";
	size_t blen = strlen(fmt) + 2 * strlen(action) + strlen(service_type) + strlen(args_xml) + 1;
	char *body = malloc(blen);
	if (!body) return HTTP_ERR_NOMEM;
	int n = snprintf(body, blen, fmt, action, service_type, args_xml, action);

	char headers[256];
	snprintf(headers, sizeof headers,
	         "Content-Type: text/xml; charset=\"utf-8\"\r\n"
	         "SOAPACTION: \"%s#%s\"\r\n", service_type, action);

	// Browse results (favorites with embedded metadata, big topologies) can
	// be a few hundred KB; everything else is tiny.
	int rc = http_request(ip, SONOS_PORT, "POST", control_path, headers, body, (size_t)n,
	                      resp, 512 * 1024, 8000); // regrouping can take >4 s
	free(body);
	int code = 0;
	if (rc == HTTP_ERR_STATUS && resp->body) {
		char c[16];
		if (xml_get(resp->body, "errorCode", c, sizeof c)) code = atoi(c);
	}
	if (upnp_error) *upnp_error = code;
	if (rc == HTTP_ERR_STATUS) netlog("%s %s -> HTTP %d upnp %d", ip, action, resp->status, code);
	return rc;
}

int soap_call(const char *ip, const char *control_path, const char *service_type,
              const char *action, const char *args_xml, http_response_t *resp,
              int *upnp_error)
{
	int err = 0;
	int rc = soap_once(ip, control_path, service_type, action, args_xml, resp, &err);

	// Retry once when it's safe and likely to help:
	//  - TIMEOUT/IO on a repeatable action: the reply may just have been lost.
	//  - UPnP 701 "transition not available": the speaker is briefly busy
	//    (switching source, regrouping) and accepts the same call a moment later.
	// (Connect failures are already retried, with a pause, in connect_timeout.)
	bool retry = ((rc == HTTP_ERR_TIMEOUT || rc == HTTP_ERR_IO) && idempotent(action)) ||
	             (rc == HTTP_ERR_STATUS && err == 701);
	if (retry && !aborting) {
		http_response_free(resp);
		svcSleepThread(rc == HTTP_ERR_STATUS ? 500 * 1000000LL : 200 * 1000000LL);
		rc = soap_once(ip, control_path, service_type, action, args_xml, resp, &err);
		netlog("%s %s retry -> %s", ip, action, rc == HTTP_OK ? "OK" : http_strerror(rc));
	}
	if (upnp_error) *upnp_error = err;
	last_upnp_error = rc == HTTP_ERR_STATUS ? err : 0;
	return rc;
}

int soap_last_upnp_error(void) { return last_upnp_error; }
