#include "diag.h"
#include "network.h"
#include "sonos_http.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static diag_log_fn out;

static void logf_(const char *fmt, ...)
{
	char line[96];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	out(line);
}

static struct sockaddr_in addr_of(const char *ip, int port)
{
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	inet_aton(ip, &sa.sin_addr);
	return sa;
}

// Non-blocking connect, reporting every signal we could use to judge success.
static void test_nonblocking(const char *ip)
{
	int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (fd < 0) { logf_("NB: socket() failed errno=%d", errno); return; }
	int fl = fcntl(fd, F_GETFL, 0);
	int frc = fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	struct sockaddr_in sa = addr_of(ip, SONOS_PORT);
	u64 t0 = osGetTime();
	errno = 0;
	int rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
	logf_("NB: fcntl=%d connect=%d errno=%d", frc, rc, errno);

	struct pollfd p = { .fd = fd, .events = POLLOUT };
	int pr = poll(&p, 1, 3000);
	u64 t1 = osGetTime();
	logf_("NB: poll=%d revents=0x%02x after %dms", pr, p.revents, (int)(t1 - t0));

	int soerr = -1; socklen_t sl = sizeof soerr;
	int gr = getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
	logf_("NB: SO_ERROR rc=%d val=%d errno=%d", gr, soerr, errno);

	errno = 0;
	int rc2 = connect(fd, (struct sockaddr *)&sa, sizeof sa);
	logf_("NB: 2nd connect=%d errno=%d (EISCONN=%d)", rc2, errno, EISCONN);

	struct sockaddr_in peer; socklen_t pl = sizeof peer;
	errno = 0;
	int gp = getpeername(fd, (struct sockaddr *)&peer, &pl);
	logf_("NB: getpeername=%d errno=%d", gp, errno);
	close(fd);
}

static void test_blocking(const char *ip)
{
	int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (fd < 0) { logf_("BL: socket() failed errno=%d", errno); return; }
	struct sockaddr_in sa = addr_of(ip, SONOS_PORT);
	u64 t0 = osGetTime();
	errno = 0;
	int rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
	logf_("BL: connect=%d errno=%d in %dms", rc, errno, (int)(osGetTime() - t0));
	if (rc == 0) {
		const char *req = "GET /status/zp HTTP/1.0\r\n\r\n";
		int s = send(fd, req, strlen(req), 0);
		char buf[64] = "";
		struct pollfd p = { .fd = fd, .events = POLLIN };
		int pr = poll(&p, 1, 3000);
		int n = pr > 0 ? recv(fd, buf, sizeof buf - 1, 0) : -1;
		if (n > 0) buf[n] = 0;
		char *nl = strchr(buf, '\r');
		if (nl) *nl = 0;
		logf_("BL: send=%d poll=%d rev=0x%02x recv=%d", s, pr, p.revents, n);
		logf_("BL: '%.40s'", buf);
	}
	close(fd);
}

static void test_ssdp(void)
{
	int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0) { logf_("UDP: socket() failed errno=%d", errno); return; }
	struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = INADDR_ANY };
	int br = bind(fd, (struct sockaddr *)&local, sizeof local);
	const char *m = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
	                "MX: 1\r\nST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";
	struct sockaddr_in mc = addr_of("239.255.255.250", 1900);
	errno = 0;
	int sr = sendto(fd, m, strlen(m), 0, (struct sockaddr *)&mc, sizeof mc);
	logf_("UDP: bind=%d sendto=%d errno=%d", br, sr, errno);

	int got = 0, polls = 0, pollerr = 0;
	unsigned revs = 0;
	u64 end = osGetTime() + 3000;
	while (osGetTime() < end) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		int pr = poll(&p, 1, 200);
		polls++;
		if (pr < 0) pollerr++;
		revs |= p.revents;
		if (pr > 0) {
			char buf[512];
			if (recvfrom(fd, buf, sizeof buf, 0, NULL, NULL) > 0) got++;
		}
	}
	// Also try without poll: one non-blocking recv (catches poll misreporting).
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	char buf[512];
	int extra = 0;
	while (recvfrom(fd, buf, sizeof buf, 0, NULL, NULL) > 0) extra++;
	logf_("UDP: replies=%d (+%d unpolled) polls=%d err=%d rev=0x%02x", got, extra, polls, pollerr, revs);
	close(fd);
}

void diag_run(const char *ip, diag_log_fn log)
{
	out = log;
	char local[16];
	network_local_ip(local);
	logf_("2DS %s  wifi=%d  target %s", local, network_status() == NET_UP, ip);
	test_blocking(ip);
	test_nonblocking(ip);
	test_ssdp();

	http_response_t r;
	u64 t0 = osGetTime();
	int rc = http_request(ip, SONOS_PORT, "GET", "/status/zp", NULL, NULL, 0, &r, 64 * 1024, 4000);
	logf_("APP http_request=%d (%s) status=%d %dms", rc, http_strerror(rc), r.status, (int)(osGetTime() - t0));
	http_response_free(&r);
	logf_("Done. Log saved to sdmc:/3ds/sonnis/debug.log");
}
