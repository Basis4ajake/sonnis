// network: Wi-Fi + socket service lifecycle and status.
#pragma once
#include <stdbool.h>

typedef enum {
	NET_DOWN,      // Wi-Fi off or not associated
	NET_UP,        // associated, have an IP
} net_status_t;

bool network_init(void);
void network_exit(void);

// Cheap status poll (one IPC call); safe to call ~once per second.
net_status_t network_status(void);

// Local IPv4 address as dotted string, or "0.0.0.0".
void network_local_ip(char out[16]);
