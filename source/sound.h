// sound: Ozzy-mode sound bites, played through the 2DS's own speaker.
//
// Drop 16-bit (or 8-bit) PCM .wav files into sdmc:/3ds/sonnis/sounds/ named
// after an event (start.wav, eleven.wav, bears.wav, sharon.wav,
// lastsong.wav, quit.wav, random1.wav, random2.wav, ...). Missing files are
// simply skipped. Needs the DSP firmware (sdmc:/3ds/dspfirm.cdc, made by the
// DSP1 homebrew); without it Sonnis stays silent.
#pragma once
#include <stdbool.h>

#define SOUND_DIR "sdmc:/3ds/sonnis/sounds"

void sound_init(void);
void sound_exit(void);

// Plays SOUND_DIR/<event>.wav (only in Ozzy mode). "random" picks one of
// random*.wav. Returns true if something started playing.
bool sound_play(const char *event);
bool sound_playing(void);

bool sound_available(void); // DSP firmware loaded
int sound_clip_count(void); // .wav files in SOUND_DIR (rescanned on init / sound_rescan)
void sound_rescan(void);
