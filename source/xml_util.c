#include "xml_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t utf8_safe_len(const char *s, size_t n)
{
	// Walk back over continuation bytes (10xxxxxx) so we cut before a lead byte.
	if (n == 0) return 0;
	size_t i = n;
	while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
	return i;
}

void str_copy_n(char *dst, const char *src, size_t n, size_t cap)
{
	if (cap == 0) return;
	if (!src) { dst[0] = 0; return; }
	if (n >= cap) n = utf8_safe_len(src, cap - 1);
	memcpy(dst, src, n);
	dst[n] = 0;
}

void str_copy(char *dst, const char *src, size_t cap)
{
	str_copy_n(dst, src, src ? strlen(src) : 0, cap);
}

static bool is_name_end(char c)
{
	return c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool xml_find(const char *src, const char *tag, const char **open_tag,
              const char **content, size_t *content_len, const char **after)
{
	if (!src) return false;
	size_t tl = strlen(tag);
	const char *p = src;
	while ((p = strchr(p, '<')) != NULL) {
		if (strncmp(p + 1, tag, tl) == 0 && is_name_end(p[1 + tl])) break;
		p++;
	}
	if (!p) return false;

	const char *gt = strchr(p, '>');
	if (!gt) return false;
	if (open_tag) *open_tag = p;

	if (gt[-1] == '/') { // self-closing <tag ... />
		if (content) *content = gt + 1;
		if (content_len) *content_len = 0;
		if (after) *after = gt + 1;
		return true;
	}

	// Find the matching close tag. Sonos never nests same-named elements in
	// the places we look, so the first close tag is the right one.
	char close[72];
	snprintf(close, sizeof close, "</%s>", tag);
	const char *end = strstr(gt + 1, close);
	if (!end) return false;
	if (content) *content = gt + 1;
	if (content_len) *content_len = (size_t)(end - (gt + 1));
	if (after) *after = end + strlen(close);
	return true;
}

bool xml_get(const char *src, const char *tag, char *out, size_t cap)
{
	const char *c; size_t n;
	out[0] = 0;
	if (!xml_find(src, tag, NULL, &c, &n, NULL)) return false;
	// Unescape into a temp copy first: escaped text is always >= decoded length,
	// so decoding a truncated escaped string could leave half an entity.
	char *tmp = malloc(n + 1);
	if (!tmp) return false;
	memcpy(tmp, c, n);
	tmp[n] = 0;
	xml_unescape(tmp);
	str_copy(out, tmp, cap);
	free(tmp);
	return true;
}

bool xml_attr(const char *open_tag, const char *name, char *out, size_t cap)
{
	out[0] = 0;
	const char *gt = strchr(open_tag, '>');
	if (!gt) return false;
	size_t nl = strlen(name);
	const char *p = open_tag;
	while ((p = strstr(p, name)) != NULL && p < gt) {
		if ((p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n') && p[nl] == '=' &&
		    (p[nl + 1] == '"' || p[nl + 1] == '\'')) {
			char q = p[nl + 1];
			const char *v = p + nl + 2;
			const char *e = strchr(v, q);
			if (!e || e > gt) return false;
			str_copy_n(out, v, (size_t)(e - v), cap);
			xml_unescape(out);
			return true;
		}
		p += nl;
	}
	return false;
}

static int put_utf8(char *o, unsigned long cp)
{
	if (cp < 0x80) { o[0] = (char)cp; return 1; }
	if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
	if (cp < 0x10000) {
		o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		o[2] = (char)(0x80 | (cp & 0x3F)); return 3;
	}
	o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

size_t xml_unescape(char *s)
{
	char *r = s, *w = s;
	while (*r) {
		if (*r != '&') { *w++ = *r++; continue; }
		if (!strncmp(r, "&lt;", 4)) { *w++ = '<'; r += 4; }
		else if (!strncmp(r, "&gt;", 4)) { *w++ = '>'; r += 4; }
		else if (!strncmp(r, "&amp;", 5)) { *w++ = '&'; r += 5; }
		else if (!strncmp(r, "&quot;", 6)) { *w++ = '"'; r += 6; }
		else if (!strncmp(r, "&apos;", 6)) { *w++ = '\''; r += 6; }
		else if (r[1] == '#') {
			char *end;
			unsigned long cp = (r[2] == 'x' || r[2] == 'X') ? strtoul(r + 3, &end, 16)
			                                                : strtoul(r + 2, &end, 10);
			if (*end == ';' && cp > 0 && cp < 0x110000) { w += put_utf8(w, cp); r = end + 1; }
			else *w++ = *r++;
		} else *w++ = *r++;
	}
	*w = 0;
	return (size_t)(w - s);
}

size_t xml_escape(const char *in, char *out, size_t cap)
{
	size_t n = 0;
	for (; *in; in++) {
		const char *rep = NULL;
		switch (*in) {
		case '<': rep = "&lt;"; break;
		case '>': rep = "&gt;"; break;
		case '&': rep = "&amp;"; break;
		case '"': rep = "&quot;"; break;
		case '\'': rep = "&apos;"; break;
		}
		size_t l = rep ? strlen(rep) : 1;
		if (n + l + 1 > cap) { if (cap) out[0] = 0; return (size_t)-1; }
		if (rep) memcpy(out + n, rep, l); else out[n] = *in;
		n += l;
	}
	if (cap) out[n] = 0;
	return n;
}
