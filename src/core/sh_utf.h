// UTF-8 and the few Unicode rules where Skill Hub 1.x relied on Qt's QString: what counts as a space, trimming, slugs,
// lengths counted in UTF-16 units, and case-insensitive comparison. Behaviour matches 1.x so names and ids stay the same.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Decodes one code point at *p (stops at `end`) and advances *p. Invalid or cut sequences give U+FFFD and advance by one
// byte, as Qt's decoder does.
uint32_t sh_utf8_next(const char **p, const char *end);
size_t sh_utf8_put(uint32_t cp, char out[4]);

bool sh_uni_is_space(uint32_t cp);    // QChar::isSpace
bool sh_uni_is_alnum(uint32_t cp);    // QChar::isLetterOrNumber
uint32_t sh_uni_lower(uint32_t cp);   // simple lowercase mapping

// Copy of `s` (n bytes) with invalid UTF-8 replaced by U+FFFD, like QString::fromUtf8.
char *sh_utf8_clean(const char *s, size_t n);

// QString::trimmed and QString::simplified on UTF-8 text (Unicode spaces count).
char *sh_trimmed(const char *s, size_t n);
char *sh_simplified(const char *s, size_t n);
bool sh_is_blank(const char *s);  // only spaces, or empty

// 1.x slugify for agent ids: lowercase, letters and digits kept, every other run becomes one '-', no '-' at the ends.
// Like Qt, which looked at UTF-16 units, a character outside the Basic Multilingual Plane counts as a separator.
char *sh_slugify(const char *s);

// The first `units` UTF-16 code units of `s` (QString::left), never cutting a surrogate pair in half.
char *sh_left_utf16(const char *s, size_t units);
size_t sh_utf16_len(const char *s);

// Case-insensitive comparison by code point (simple lowercase); equal names fall back to byte order.
int sh_casecmp(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
