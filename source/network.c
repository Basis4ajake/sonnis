#include "network.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>

#define SOC_ALIGN 0x1000
#define SOC_BUFFERSIZE 0x100000

static u32 *soc_buf;
static bool soc_ok, ac_ok;

bool network_init(void)
{
	ac_ok = R_SUCCEEDED(acInit());
	soc_buf = memalign(SOC_ALIGN, SOC_BUFFERSIZE);
	if (!soc_buf) return false;
	soc_ok = R_SUCCEEDED(socInit(soc_buf, SOC_BUFFERSIZE));
	return soc_ok;
}

void network_exit(void)
{
	if (soc_ok) socExit();
	free(soc_buf);
	soc_buf = NULL;
	if (ac_ok) acExit();
	soc_ok = ac_ok = false;
}

net_status_t network_status(void)
{
	u32 wifi = 0;
	if (!ac_ok || R_FAILED(ACU_GetWifiStatus(&wifi)) || wifi == 0) return NET_DOWN;
	return gethostid() != 0 ? NET_UP : NET_DOWN;
}

void network_local_ip(char out[16])
{
	struct in_addr a = { .s_addr = (in_addr_t)gethostid() };
	snprintf(out, 16, "%s", inet_ntoa(a));
}
