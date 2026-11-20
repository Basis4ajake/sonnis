// netlog: a small ring buffer of network events/failures, viewable on the
// device (Settings > Network log) and appended to sdmc:/3ds/sonnis/debug.log.
// Thread-safe; called from the worker, read by the UI.
#pragma once

#define NETLOG_LINES 18
#define NETLOG_WIDTH 112

void netlog_init(void);
void netlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Copies up to max most-recent lines (oldest first). Returns count.
int netlog_copy(char out[][NETLOG_WIDTH], int max);
void netlog_clear(void);
