#include "sh_utf.h"

#include <string.h>

#include "sh_str.h"
#include "sh_unicode_tables.h"

uint32_t sh_utf8_next(const char **pp, const char *end) {
    const unsigned char *p = (const unsigned char *)*pp;
    const unsigned char *e = (const unsigned char *)end;
    if (p >= e) return 0;
    const unsigned c = p[0];
    if (c < 0x80) {
        *pp += 1;
        return c;
    }
    int n;
    uint32_t cp, min;
    if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; min = 0x10000; }
    else { *pp += 1; return 0xFFFD; }
    if (e - p <= n) { *pp += 1; return 0xFFFD; }
    for (int i = 1; i <= n; ++i) {
        if ((p[i] & 0xC0) != 0x80) { *pp += 1; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { *pp += 1; return 0xFFFD; }
    *pp += n + 1;
    return cp;
}

size_t sh_utf8_put(uint32_t cp, char out[4]) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static bool in_ranges(const uint32_t (*r)[2], size_t n, uint32_t cp) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (cp < r[mid][0]) hi = mid;
        else if (cp > r[mid][1]) lo = mid + 1;
        else return true;
    }
    return false;
}

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

bool sh_uni_is_space(uint32_t cp) { return in_ranges(sh_uni_space_table, COUNT(sh_uni_space_table), cp); }
bool sh_uni_is_alnum(uint32_t cp) { return in_ranges(sh_uni_alnum_table, COUNT(sh_uni_alnum_table), cp); }

uint32_t sh_uni_lower(uint32_t cp) {
    if (cp < 0x80) return cp >= 'A' && cp <= 'Z' ? cp + 32 : cp;
    size_t lo = 0, hi = COUNT(sh_uni_lower_table);
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (cp < sh_uni_lower_table[mid][0]) hi = mid;
        else if (cp > sh_uni_lower_table[mid][0]) lo = mid + 1;
        else return sh_uni_lower_table[mid][1];
    }
    return cp;
}

char *sh_utf8_clean(const char *s, size_t n) {
    sh_buf b = SH_BUF_INIT;
    const char *p = s, *end = s + n;
    while (p < end) {
        const char *start = p;
        const uint32_t cp = sh_utf8_next(&p, end);
        if (cp == 0xFFFD && !(p - start == 3 && memcmp(start, "\xEF\xBF\xBD", 3) == 0)) {
            sh_buf_append(&b, "\xEF\xBF\xBD", 3);
        } else {
            sh_buf_append(&b, start, (size_t)(p - start));
        }
    }
    return sh_buf_steal(&b);
}

char *sh_trimmed(const char *s, size_t n) {
    const char *p = s, *end = s + n;
    const char *first = end, *last = s;  // [first, last) is what stays
    while (p < end) {
        const char *start = p;
        const uint32_t cp = sh_utf8_next(&p, end);
        if (!sh_uni_is_space(cp)) {
            if (first == end) first = start;
            last = p;
        }
    }
    return first < last ? sh_strndup(first, (size_t)(last - first)) : sh_strdup("");
}

char *sh_simplified(const char *s, size_t n) {
    sh_buf b = SH_BUF_INIT;
    bool pending = false;
    const char *p = s, *end = s + n;
    while (p < end) {
        const char *start = p;
        const uint32_t cp = sh_utf8_next(&p, end);
        if (sh_uni_is_space(cp)) {
            pending = b.len > 0;
            continue;
        }
        if (pending) sh_buf_putc(&b, ' ');
        pending = false;
        sh_buf_append(&b, start, (size_t)(p - start));
    }
    return sh_buf_steal(&b);
}

bool sh_is_blank(const char *s) {
    const char *p = s, *end = s + strlen(s);
    while (p < end)
        if (!sh_uni_is_space(sh_utf8_next(&p, end))) return false;
    return true;
}

char *sh_slugify(const char *s) {
    sh_buf b = SH_BUF_INIT;
    const char *p = s, *end = s + strlen(s);
    while (p < end) {
        const uint32_t cp = sh_uni_lower(sh_utf8_next(&p, end));
        if (cp <= 0xFFFF && sh_uni_is_alnum(cp)) {
            char u[4];
            sh_buf_append(&b, u, sh_utf8_put(cp, u));
        } else if (b.len > 0 && b.data[b.len - 1] != '-') {
            sh_buf_putc(&b, '-');
        }
    }
    while (b.len > 0 && b.data[b.len - 1] == '-') b.data[--b.len] = '\0';
    return sh_buf_steal(&b);
}

size_t sh_utf16_len(const char *s) {
    size_t units = 0;
    const char *p = s, *end = s + strlen(s);
    while (p < end) units += sh_utf8_next(&p, end) > 0xFFFF ? 2 : 1;
    return units;
}

char *sh_left_utf16(const char *s, size_t units) {
    size_t used = 0;
    const char *p = s, *end = s + strlen(s);
    while (p < end) {
        const char *start = p;
        const size_t w = sh_utf8_next(&p, end) > 0xFFFF ? 2 : 1;
        if (used + w > units) return sh_strndup(s, (size_t)(start - s));
        used += w;
    }
    return sh_strdup(s);
}

int sh_casecmp(const char *a, const char *b) {
    const char *p = a, *pe = a + strlen(a), *q = b, *qe = b + strlen(b);
    while (p < pe && q < qe) {
        const uint32_t x = sh_uni_lower(sh_utf8_next(&p, pe)), y = sh_uni_lower(sh_utf8_next(&q, qe));
        if (x != y) return x < y ? -1 : 1;
    }
    if (p < pe) return 1;
    if (q < qe) return -1;
    return strcmp(a, b);
}
