// albumart: downloads cover art from a speaker and decodes it into a fixed
// 256x256 RGBA8 buffer ready to swizzle into a GPU texture.
#pragma once
#include <3ds/types.h>

#define ART_SIZE 256

// art_uri may be relative ("/getaa?...", served by the speaker) or an
// absolute http:// URL. Returns a malloc'd ART_SIZE*ART_SIZE buffer of
// 0xRRGGBBAA pixels, or NULL on failure (https, network error, bad image).
u32 *albumart_fetch(const char *speaker_ip, const char *art_uri);
