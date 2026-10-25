#include "sh_str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void out_of_memory(void) {
    fputs("skillhub: out of memory\n", stderr);
    abort();
}

void *sh_xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) out_of_memory();
    return p;
}

void *sh_xcalloc(size_t count, size_t n) {
    void *p = calloc(count ? count : 1, n ? n : 1);
    if (!p) out_of_memory();
    return p;
}

void *sh_xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) out_of_memory();
    return q;
}

char *sh_strdup(const char *s) { return sh_strndup(s ? s : "", s ? strlen(s) : 0); }

char *sh_strndup(const char *s, size_t n) {
    char *p = sh_xmalloc(n + 1);
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *sh_strfmt(const char *fmt, ...) {
    sh_buf b = SH_BUF_INIT;
    va_list ap;
    va_start(ap, fmt);
    sh_buf_vprintf(&b, fmt, ap);
    va_end(ap);
    return sh_buf_steal(&b);
}

bool sh_starts_with(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

bool sh_ends_with(const char *s, const char *suffix) {
    const size_t n = strlen(s), m = strlen(suffix);
    return n >= m && memcmp(s + n - m, suffix, m) == 0;
}

bool sh_streq(const char *a, const char *b) { return strcmp(a ? a : "", b ? b : "") == 0; }

// ---- buffer ----

static void reserve(sh_buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + extra + 1) cap *= 2;
    b->data = sh_xrealloc(b->data, cap);
    b->cap = cap;
}

void sh_buf_append(sh_buf *b, const void *data, size_t n) {
    reserve(b, n);
    if (n) memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void sh_buf_puts(sh_buf *b, const char *s) { sh_buf_append(b, s, strlen(s)); }

void sh_buf_putc(sh_buf *b, char c) { sh_buf_append(b, &c, 1); }

void sh_buf_vprintf(sh_buf *b, const char *fmt, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    const int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) return;
    reserve(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
}

void sh_buf_printf(sh_buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sh_buf_vprintf(b, fmt, ap);
    va_end(ap);
}

void sh_buf_clear(sh_buf *b) {
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

void sh_buf_free(sh_buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

const char *sh_buf_str(const sh_buf *b) { return b->data ? b->data : ""; }

char *sh_buf_steal(sh_buf *b) {
    char *s = b->data ? b->data : sh_strdup("");
    b->data = NULL;
    b->len = b->cap = 0;
    return s;
}

// ---- string list ----

void sh_strv_take(sh_strv *v, char *s) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = sh_xrealloc(v->items, v->cap * sizeof *v->items);
    }
    v->items[v->len++] = s;
}

void sh_strv_push(sh_strv *v, const char *s) { sh_strv_take(v, sh_strdup(s)); }

ptrdiff_t sh_strv_index(const sh_strv *v, const char *s) {
    for (size_t i = 0; i < v->len; ++i)
        if (strcmp(v->items[i], s) == 0) return (ptrdiff_t)i;
    return -1;
}

bool sh_strv_contains(const sh_strv *v, const char *s) { return sh_strv_index(v, s) >= 0; }

// a merge sort, not qsort: qsort has no way to pass the comparison along except a global, and threads sort at the same
// time. It is stable too, so names that compare equal keep the order the folder listed them in.
static void merge_sort(char **a, char **tmp, size_t n, int (*cmp)(const char *, const char *)) {
    if (n < 2) return;
    const size_t mid = n / 2;
    merge_sort(a, tmp, mid, cmp);
    merge_sort(a + mid, tmp, n - mid, cmp);
    size_t i = 0, j = mid, k = 0;
    while (i < mid && j < n) tmp[k++] = cmp(a[j], a[i]) < 0 ? a[j++] : a[i++];
    while (i < mid) tmp[k++] = a[i++];
    while (j < n) tmp[k++] = a[j++];
    memcpy(a, tmp, n * sizeof *a);
}

void sh_strv_sort(sh_strv *v, int (*cmp)(const char *, const char *)) {
    if (v->len < 2) return;
    char **tmp = sh_xcalloc(v->len, sizeof *tmp);
    merge_sort(v->items, tmp, v->len, cmp ? cmp : strcmp);
    free(tmp);
}

void sh_strv_clear(sh_strv *v) {
    for (size_t i = 0; i < v->len; ++i) free(v->items[i]);
    v->len = 0;
}

void sh_strv_free(sh_strv *v) {
    sh_strv_clear(v);
    free(v->items);
    v->items = NULL;
    v->cap = 0;
}

char *sh_strv_join(const sh_strv *v, const char *sep) {
    sh_buf b = SH_BUF_INIT;
    for (size_t i = 0; i < v->len; ++i) {
        if (i) sh_buf_puts(&b, sep);
        sh_buf_puts(&b, v->items[i]);
    }
    return sh_buf_steal(&b);
}

void sh_split(const char *s, size_t n, char sep, bool skip_empty, sh_strv *out) {
    size_t start = 0;
    for (size_t i = 0; i <= n; ++i) {
        if (i == n || s[i] == sep) {
            if (i > start || !skip_empty) sh_strv_take(out, sh_strndup(s + start, i - start));
            start = i + 1;
        }
    }
}
