// sonos_http: minimal blocking HTTP/1.1 client plus UPnP SOAP helper.
// Every call has a hard timeout so a sleeping or unplugged speaker can never
// wedge the worker thread. Only ever called from the controller thread.
#pragma once
#include <stdbool.h>
#include <stddef.h>

struct sockaddr_in;

#define SONOS_PORT 1400

enum {
	HTTP_OK = 0,
	HTTP_ERR_SOCKET = -1,
	HTTP_ERR_CONNECT = -2,
	HTTP_ERR_TIMEOUT = -3,
	HTTP_ERR_IO = -4,
	HTTP_ERR_PROTO = -5,
	HTTP_ERR_NOMEM = -6,
	HTTP_ERR_TOOBIG = -7,
	HTTP_ERR_STATUS = -8, // non-2xx response; see resp->status
	HTTP_ERR_ABORTED = -9, // app is shutting down
};

// Shutdown support: after net_abort_all(), every wait in the network code
// (connect, reply, discovery, scan) gives up within ~250 ms. Without this the
// app could tear down the socket service while the worker was still inside a
// 7 s connect, and the worker's close() would then crash (seen on device).
void net_abort_all(void);
bool net_aborting(void);

typedef struct {
	int status;     // HTTP status code
	char *body;     // NUL-terminated, malloc'd (free with http_response_free)
	size_t len;
} http_response_t;

int http_request(const char *ip, int port, const char *method, const char *path,
                 const char *extra_headers, const char *body, size_t body_len,
                 http_response_t *resp, size_t max_bytes, int timeout_ms);

void http_response_free(http_response_t *r);

// Splits "http://host:port/path" into its parts. https is rejected because the
// 2DS has no practical TLS stack here.
bool http_parse_url(const char *url, char *host, size_t host_cap, int *port,
                    char *path, size_t path_cap);

// Performs a SOAP action. `args_xml` is the already-escaped inner argument
// XML, e.g. "<InstanceID>0</InstanceID><Speed>1</Speed>".
// On HTTP 500 the UPnP error code (if any) is returned in *upnp_error.
int soap_call(const char *ip, const char *control_path, const char *service_type,
              const char *action, const char *args_xml, http_response_t *resp,
              int *upnp_error);

// For a non-blocking socket with a connect in flight: 1 = connected,
// 0 = still connecting, -1 = failed.
int sock_connect_status(int fd, const struct sockaddr_in *sa);

// UPnP error code of the most recent soap_call that got HTTP 500 (else 0).
// Only meaningful on the worker thread, which makes all SOAP calls.
int soap_last_upnp_error(void);

// Human-readable text for an HTTP_ERR_* code.
const char *http_strerror(int err);
