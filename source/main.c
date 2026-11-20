// Sonnis — a Sonos remote for the Nintendo 2DS / 3DS family.
//
// Threads:
//   main    : input + rendering at 60 fps (ui.c), never blocks on the network
//   worker  : all speaker I/O, polling, discovery, album art (controller.c)
#include <3ds.h>
#include <stdlib.h>
#include <time.h>

#include "controller.h"
#include "input.h"
#include "network.h"
#include "netlog.h"
#include "settings.h"
#include "sound.h"
#include "ozzy.h"
#include "ui.h"

static aptHookCookie apt_cookie;

static void on_apt(APT_HookType hook, void *param)
{
	// Sockets don't survive sleep reliably and the Wi-Fi may have changed:
	// re-check the network and rediscover after waking.
	if (hook == APTHOOK_ONWAKEUP || hook == APTHOOK_ONRESTORE) ctrl_on_wake();
}

int main(int argc, char **argv)
{
	gfxInitDefault();
	osSetSpeedupEnable(true); // New 3DS/2DS XL: 804 MHz; ignored on 2DS

	settings_load(&g_settings);
	settings_apply_theme(&g_settings);

	netlog_init();
	network_init();
	ui_init();
	input_init();
	ctrl_start();
	aptHook(&apt_cookie, on_apt, NULL);
	srand((unsigned)time(NULL));
	g_ozzy = g_settings.ozzy;
	sound_init();
	sound_play("start");

	input_t in = { 0 };
	while (aptMainLoop()) {
		input_poll(&in);
		if (!ui_frame(&in)) break;
	}

	aptUnhook(&apt_cookie);
	bool bye = sound_play("quit");
	bool worker_stopped = ctrl_stop();
	// Let the goodbye finish (up to 5 s) before tearing audio down.
	for (int i = 0; bye && sound_playing() && i < 300; i++) gspWaitForVBlank();
	sound_exit();
	settings_save(&g_settings);
	ui_exit();
	// Removing the socket service under a live worker crashes its next
	// close() (that was the 1.0 crash). Leaking it at exit is harmless.
	if (worker_stopped) network_exit();
	gfxExit();
	return 0;
}
