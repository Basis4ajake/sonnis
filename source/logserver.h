// logserver: while the Network log screen is open, serves the debug log
// read-only over HTTP on the LAN (http://<2ds-ip>:8080/) so it can be pulled
// from a PC without removing the SD card. Also: GET /dumps lists Luma3DS
// ARM11 crash dumps, GET /dump/<name> downloads one. PUT /install saves the
// app to sdmc:/3ds/sonnis/sonnis.3dsx and PUT /sounds/<name>.wav adds a sound
// bite (curl -T file http://<2ds-ip>:8080/install). Runs on its own thread.
#pragma once
#include <stdbool.h>

#define LOGSERVER_PORT 8080

void logserver_start(void);
void logserver_stop(void);
bool logserver_running(void);
int logserver_hits(void); // number of times the log was fetched
