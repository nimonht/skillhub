// Growable buffers and string lists: the string tools the core is built on. Strings are UTF-8 and NUL-terminated.
// Allocation failures end the program (as git does): every function here either succeeds or does not return.
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__MINGW32__)
#define SH_PRINTF(fmt, args) __attribute__((format(gnu_printf, fmt, args)))  // C99 stdio (__USE_MINGW_ANSI_STDIO)
#elif defined(__GNUC__) || defined(__clang__)
#define SH_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define SH_PRINTF(fmt, args)
#endif

void *sh_xmalloc(size_t n);
void *sh_xcalloc(size_t count, size_t n);
void *sh_xrealloc(void *p, size_t n);
char *sh_strdup(const char *s);
char *sh_strndup(const char *s, size_t n);
char *sh_strfmt(const char *fmt, ...) SH_PRINTF(1, 2);

bool sh_starts_with(const char *s, const char *prefix);
bool sh_ends_with(const char *s, const char *suffix);
bool sh_streq(const char *a, const char *b);  // NULL-safe; NULL equals ""

// ---- buffer ----

typedef struct sh_buf {
    char *data;  // always NUL-terminated once anything was appended; NULL when empty and never used
    size_t len;
    size_t cap;
} sh_buf;

#define SH_BUF_INIT {NULL, 0, 0}

void sh_buf_append(sh_buf *b, const void *data, size_t n);
void sh_buf_puts(sh_buf *b, const char *s);
void sh_buf_putc(sh_buf *b, char c);
void sh_buf_printf(sh_buf *b, const char *fmt, ...) SH_PRINTF(2, 3);
void sh_buf_vprintf(sh_buf *b, const char *fmt, va_list ap);
void sh_buf_clear(sh_buf *b);
void sh_buf_free(sh_buf *b);
const char *sh_buf_str(const sh_buf *b);  // "" when empty
char *sh_buf_steal(sh_buf *b);            // the contents as a malloc'd string; the buffer is empty afterwards

// ---- string list ----

typedef struct sh_strv {
    char **items;
    size_t len;
    size_t cap;
} sh_strv;

#define SH_STRV_INIT {NULL, 0, 0}

void sh_strv_push(sh_strv *v, const char *s);  // copies
void sh_strv_take(sh_strv *v, char *s);        // takes ownership of a malloc'd string
bool sh_strv_contains(const sh_strv *v, const char *s);
ptrdiff_t sh_strv_index(const sh_strv *v, const char *s);
void sh_strv_sort(sh_strv *v, int (*cmp)(const char *, const char *));
void sh_strv_clear(sh_strv *v);
void sh_strv_free(sh_strv *v);
char *sh_strv_join(const sh_strv *v, const char *sep);

// Split `s` at every `sep` byte; empty parts are kept unless `skip_empty`.
void sh_split(const char *s, size_t n, char sep, bool skip_empty, sh_strv *out);

#ifdef __cplusplus
}
#endif
