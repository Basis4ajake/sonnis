#include "albumart.h"
#include "sonos_http.h"

#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

u32 *albumart_fetch(const char *speaker_ip, const char *art_uri)
{
	char host[64], path[512];
	int port;
	if (!art_uri || !art_uri[0]) return NULL;
	if (art_uri[0] == '/') {
		strncpy(host, speaker_ip, sizeof host - 1); host[sizeof host - 1] = 0;
		port = SONOS_PORT;
		strncpy(path, art_uri, sizeof path - 1); path[sizeof path - 1] = 0;
	} else if (!http_parse_url(art_uri, host, sizeof host, &port, path, sizeof path)) {
		return NULL; // https or unknown scheme
	}

	http_response_t r;
	if (http_request(host, port, "GET", path, NULL, NULL, 0, &r, 2 * 1024 * 1024, 5000) != HTTP_OK) {
		http_response_free(&r);
		return NULL;
	}

	int w, h, ch;
	unsigned char *img = stbi_load_from_memory((const stbi_uc *)r.body, (int)r.len, &w, &h, &ch, 4);
	http_response_free(&r);
	if (!img) return NULL;

	u32 *out = malloc(ART_SIZE * ART_SIZE * 4);
	if (!out) { stbi_image_free(img); return NULL; }

	// Box-filter (area average) resample to 256x256: much nicer than nearest
	// for the typical 600-1000px covers, and cheap enough at this size.
	for (int y = 0; y < ART_SIZE; y++) {
		int y0 = y * h / ART_SIZE, y1 = (y + 1) * h / ART_SIZE;
		if (y1 <= y0) y1 = y0 + 1;
		for (int x = 0; x < ART_SIZE; x++) {
			int x0 = x * w / ART_SIZE, x1 = (x + 1) * w / ART_SIZE;
			if (x1 <= x0) x1 = x0 + 1;
			u32 sr = 0, sg = 0, sb = 0, n = 0;
			for (int yy = y0; yy < y1; yy++) {
				const unsigned char *row = img + ((size_t)yy * w + x0) * 4;
				for (int xx = x0; xx < x1; xx++, row += 4) { sr += row[0]; sg += row[1]; sb += row[2]; n++; }
			}
			out[y * ART_SIZE + x] = ((sr / n) << 24) | ((sg / n) << 16) | ((sb / n) << 8) | 0xFF;
		}
	}
	stbi_image_free(img);
	return out;
}
