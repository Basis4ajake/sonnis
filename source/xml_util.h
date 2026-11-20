// xml_util: tiny, allocation-free helpers for the flat XML that Sonos speaks.
// This is deliberately NOT a general XML parser: UPnP responses are small,
// predictable documents, and a full DOM would waste the 2DS's limited RAM.
#pragma once
#include <stdbool.h>
#include <stddef.h>

// Finds the element <tag ...>content</tag> starting at src.
// On success *content/*content_len describe the raw (still escaped) content,
// *open_tag points at the '<' of the opening tag and *after points just past
// the closing tag, so callers can iterate siblings. Self-closing tags yield
// an empty content. `tag` includes any namespace prefix, e.g. "dc:title".
bool xml_find(const char *src, const char *tag, const char **open_tag,
              const char **content, size_t *content_len, const char **after);

// Copies the unescaped text content of the first <tag> into out.
// Returns false (and sets out to "") when missing.
bool xml_get(const char *src, const char *tag, char *out, size_t cap);

// Reads attribute `name` from the opening tag that starts at open_tag.
bool xml_attr(const char *open_tag, const char *name, char *out, size_t cap);

// Decodes &lt; &gt; &amp; &quot; &apos; &#N; &#xN; in place. Returns new length.
size_t xml_unescape(char *s);

// Escapes text for inclusion inside an XML element. Returns bytes written
// (excluding NUL), or (size_t)-1 if it did not fit.
size_t xml_escape(const char *in, char *out, size_t cap);

// Bounded string copy that always NUL-terminates; truncates on a UTF-8
// character boundary so the font renderer never sees half a glyph.
void str_copy(char *dst, const char *src, size_t cap);
void str_copy_n(char *dst, const char *src, size_t n, size_t cap);
