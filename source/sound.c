#include "sound.h"
#ifdef SONNIS_SOUND // see sound.h: off by default
#include "ozzy.h"
#include "netlog.h"

#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define CHANNEL 0
#define MAX_CLIP_BYTES (3 * 1024 * 1024) // ~17 s of 44.1 kHz stereo

static bool ready;
static int clip_count, random_count;
static ndspWaveBuf wave;
static void *pcm; // linear memory owned by the current clip

static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u16 rd16(const u8 *p) { return p[0] | p[1] << 8; }

void sound_rescan(void)
{
	clip_count = random_count = 0;
	DIR *d = opendir(SOUND_DIR);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		size_t n = strlen(e->d_name);
		if (n < 5 || strcasecmp(e->d_name + n - 4, ".wav")) continue;
		clip_count++;
		if (!strncasecmp(e->d_name, "random", 6)) random_count++;
	}
	closedir(d);
}

void sound_init(void)
{
	mkdir(SOUND_DIR, 0777);
	sound_rescan();
	// Fails without sdmc:/3ds/dspfirm.cdc; then every call is a no-op.
	if (R_FAILED(ndspInit())) { netlog("sound: no DSP firmware, sound bites off"); return; }
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ready = true;
}

static void stop_current(void)
{
	ndspChnWaveBufClear(CHANNEL);
	if (pcm) { linearFree(pcm); pcm = NULL; }
}

void sound_exit(void)
{
	if (!ready) return;
	stop_current();
	ndspExit();
	ready = false;
}

bool sound_available(void) { return ready; }
int sound_clip_count(void) { return clip_count; }
bool sound_playing(void) { return ready && pcm && ndspChnIsPlaying(CHANNEL); }

// Loads a RIFF/WAVE PCM file into linear memory and starts it.
static bool play_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	u8 h[12];
	if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fclose(f); return false; }

	u16 fmt = 0, chans = 0, bits = 0;
	u32 rate = 0, data_len = 0;
	u8 ch[8];
	// Walk the chunks until "data"; "fmt " must come before it.
	while (fread(ch, 1, 8, f) == 8) {
		u32 len = rd32(ch + 4);
		if (!memcmp(ch, "fmt ", 4)) {
			u8 b[16];
			if (len < 16 || fread(b, 1, 16, f) != 16) break;
			fmt = rd16(b); chans = rd16(b + 2); rate = rd32(b + 4); bits = rd16(b + 14);
			fseek(f, (long)(len - 16 + (len & 1)), SEEK_CUR);
		} else if (!memcmp(ch, "data", 4)) {
			data_len = len;
			break;
		} else {
			fseek(f, (long)(len + (len & 1)), SEEK_CUR); // chunks are word aligned
		}
	}
	if (fmt != 1 || (chans != 1 && chans != 2) || (bits != 8 && bits != 16) || !data_len || !rate) {
		netlog("sound: %s is not 8/16-bit PCM wav", path);
		fclose(f);
		return false;
	}
	if (data_len > MAX_CLIP_BYTES) data_len = MAX_CLIP_BYTES;

	stop_current();
	pcm = linearAlloc(data_len);
	if (!pcm) { fclose(f); return false; }
	data_len = (u32)fread(pcm, 1, data_len, f);
	fclose(f);
	if (bits == 8) // WAV 8-bit is unsigned; the DSP wants signed
		for (u32 i = 0; i < data_len; i++) ((u8 *)pcm)[i] ^= 0x80;

	u32 frame = chans * (bits / 8);
	ndspChnReset(CHANNEL);
	ndspChnSetInterp(CHANNEL, NDSP_INTERP_LINEAR);
	ndspChnSetRate(CHANNEL, (float)rate);
	u16 format = bits == 16 ? (chans == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16)
	                        : (chans == 2 ? NDSP_FORMAT_STEREO_PCM8 : NDSP_FORMAT_MONO_PCM8);
	ndspChnSetFormat(CHANNEL, format);
	float mix[12] = { 1.0f, 1.0f };
	ndspChnSetMix(CHANNEL, mix);

	DSP_FlushDataCache(pcm, data_len);
	memset(&wave, 0, sizeof wave);
	wave.data_vaddr = pcm;
	wave.nsamples = data_len / frame;
	ndspChnWaveBufAdd(CHANNEL, &wave);
	return true;
}

bool sound_play(const char *event)
{
	if (!ready || !g_ozzy || !clip_count) return false;
	char path[128];
	if (!strcmp(event, "random")) {
		if (!random_count) return false;
		// Pick the k-th random*.wav in directory order.
		int k = rand() % random_count;
		DIR *d = opendir(SOUND_DIR);
		if (!d) return false;
		struct dirent *e;
		path[0] = 0;
		while ((e = readdir(d))) {
			size_t n = strlen(e->d_name);
			if (n < 5 || strcasecmp(e->d_name + n - 4, ".wav") || strncasecmp(e->d_name, "random", 6)) continue;
			if (k-- == 0) { snprintf(path, sizeof path, SOUND_DIR "/%s", e->d_name); break; }
		}
		closedir(d);
		if (!path[0]) return false;
	} else {
		snprintf(path, sizeof path, SOUND_DIR "/%s.wav", event);
	}
	return play_file(path);
}
#endif // SONNIS_SOUND
