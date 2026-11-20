// sound: Ozzy-mode sound bites, played through the 2DS's own speaker.
//
// Drop 16-bit (or 8-bit) PCM .wav files into sdmc:/3ds/sonnis/sounds/ named
// after an event (start.wav, eleven.wav, bears.wav, sharon.wav,
// lastsong.wav, quit.wav, random1.wav, random2.wav, ...). Missing files are
// simply skipped. Needs the DSP firmware (sdmc:/3ds/dspfirm.cdc, made by the
// DSP1 homebrew); without it Sonnis stays silent.
//
// Off by default: built only with `make SOUND=1` (defines SONNIS_SOUND).
// Without it every function below is an empty inline stub, so callers need
// no #ifdefs and release builds contain no audio code or DSP use at all.
#pragma once
#include <stdbool.h>

#define SOUND_DIR "sdmc:/3ds/sonnis/sounds"

#ifdef SONNIS_SOUND
void sound_init(void);
void sound_exit(void);

// Plays SOUND_DIR/<event>.wav (only in Ozzy mode). "random" picks one of
// random*.wav. Returns true if something started playing.
bool sound_play(const char *event);
bool sound_playing(void);

bool sound_available(void); // DSP firmware loaded
int sound_clip_count(void); // .wav files in SOUND_DIR (rescanned on init / sound_rescan)
void sound_rescan(void);
#else
static inline void sound_init(void) {}
static inline void sound_exit(void) {}
static inline bool sound_play(const char *event) { (void)event; return false; }
static inline bool sound_playing(void) { return false; }
static inline bool sound_available(void) { return false; }
static inline int sound_clip_count(void) { return 0; }
static inline void sound_rescan(void) {}
#endif
