#include "netlog.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static char ring[NETLOG_LINES][NETLOG_WIDTH];
static int head, count;
static LightLock lock;
static u64 t0;

void netlog_init(void)
{
	LightLock_Init(&lock);
	t0 = osGetTime();
}

void netlog(const char *fmt, ...)
{
	char msg[NETLOG_WIDTH - 10];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);

	char line[NETLOG_WIDTH];
	unsigned secs = (unsigned)((osGetTime() - t0) / 1000);
	snprintf(line, sizeof line, "%4u.%u %s", secs, (unsigned)((osGetTime() - t0) / 100 % 10), msg);

	LightLock_Lock(&lock);
	memcpy(ring[(head + count) % NETLOG_LINES], line, sizeof line);
	if (count < NETLOG_LINES) count++;
	else head = (head + 1) % NETLOG_LINES;
	LightLock_Unlock(&lock);

	mkdir("sdmc:/3ds", 0777);
	mkdir("sdmc:/3ds/sonnis", 0777);
	FILE *f = fopen("sdmc:/3ds/sonnis/debug.log", "a");
	if (f) { fprintf(f, "%s\n", line); fclose(f); }
}

int netlog_copy(char out[][NETLOG_WIDTH], int max)
{
	LightLock_Lock(&lock);
	int n = count < max ? count : max;
	int start = head + (count - n);
	for (int i = 0; i < n; i++) memcpy(out[i], ring[(start + i) % NETLOG_LINES], NETLOG_WIDTH);
	LightLock_Unlock(&lock);
	return n;
}

void netlog_clear(void)
{
	LightLock_Lock(&lock);
	head = count = 0;
	LightLock_Unlock(&lock);
}
