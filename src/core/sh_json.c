#include "sh_json.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sh_fs.h"
#include "sh_str.h"

#define READ_FLAGS YYJSON_READ_ALLOW_INVALID_UNICODE  // yyjson flags are const variables: MSVC rejects them in static initializers

yyjson_doc *sh_json_parse(const char *data, size_t n) {
    yyjson_doc *doc = yyjson_read(data, n, READ_FLAGS);
    if (doc && !yyjson_is_obj(yyjson_doc_get_root(doc)) && !yyjson_is_arr(yyjson_doc_get_root(doc))) {
        yyjson_doc_free(doc);
        return NULL;
    }
    return doc;
}

sh_json_status sh_json_read(const char *path, size_t max, yyjson_doc **doc) {
    *doc = NULL;
    if (!sh_lexists(path)) return SH_JSON_MISSING;
    sh_buf b = SH_BUF_INIT;
    if (!sh_read_file(path, max, &b)) {
        sh_buf_free(&b);
        return SH_JSON_UNREADABLE;
    }
    yyjson_doc *d = yyjson_read(sh_buf_str(&b), b.len, READ_FLAGS);
    sh_buf_free(&b);
    if (!d || !yyjson_is_obj(yyjson_doc_get_root(d))) {
        yyjson_doc_free(d);
        return SH_JSON_INVALID;
    }
    *doc = d;
    return SH_JSON_OK;
}

char *sh_json_string(yyjson_mut_doc *doc, bool pretty) {
    size_t len = 0;
    yyjson_write_err err;
    char *s = yyjson_mut_write_opts(doc, (pretty ? YYJSON_WRITE_PRETTY_TWO_SPACES : 0) | YYJSON_WRITE_ALLOW_INVALID_UNICODE,
                                    NULL, &len, &err);
    if (!s) return sh_strdup("{}");
    char *out = sh_strndup(s, len);
    free(s);
    return out;
}

bool sh_json_write(const char *path, yyjson_mut_doc *doc, bool two_spaces, bool final_newline) {
    size_t len = 0;
    yyjson_write_err err;
    const yyjson_write_flag flags = (two_spaces ? YYJSON_WRITE_PRETTY_TWO_SPACES : YYJSON_WRITE_PRETTY) |
                                    YYJSON_WRITE_ALLOW_INVALID_UNICODE;
    char *s = yyjson_mut_write_opts(doc, flags, NULL, &len, &err);
    if (!s) return false;
    sh_buf b = SH_BUF_INIT;
    sh_buf_append(&b, s, len);
    free(s);
    if (final_newline) sh_buf_putc(&b, '\n');
    const bool ok = sh_write_atomic(path, b.data, b.len);
    sh_buf_free(&b);
    return ok;
}

const char *sh_json_str(yyjson_val *obj, const char *key) {
    yyjson_val *v = obj ? yyjson_obj_get(obj, key) : NULL;
    return yyjson_is_str(v) ? yyjson_get_str(v) : "";
}

bool sh_json_bool(yyjson_val *obj, const char *key) {
    yyjson_val *v = obj ? yyjson_obj_get(obj, key) : NULL;
    return yyjson_is_bool(v) && yyjson_get_bool(v);
}

// like Qt's QJsonValue::toInt: a number that is a whole value in range, else the fallback
long long sh_json_int(yyjson_val *obj, const char *key, long long fallback) {
    yyjson_val *v = obj ? yyjson_obj_get(obj, key) : NULL;
    if (yyjson_is_sint(v)) return (long long)yyjson_get_sint(v);
    if (yyjson_is_uint(v)) {
        const uint64_t u = yyjson_get_uint(v);
        return u <= (uint64_t)LLONG_MAX ? (long long)u : fallback;
    }
    if (yyjson_is_real(v)) {
        const double r = yyjson_get_real(v);
        if (r >= -9.2e18 && r <= 9.2e18 && (double)(long long)r == r) return (long long)r;
    }
    return fallback;
}

yyjson_val *sh_json_obj(yyjson_val *obj, const char *key) {
    yyjson_val *v = obj ? yyjson_obj_get(obj, key) : NULL;
    return yyjson_is_obj(v) ? v : NULL;
}

yyjson_val *sh_json_arr(yyjson_val *obj, const char *key) {
    yyjson_val *v = obj ? yyjson_obj_get(obj, key) : NULL;
    return yyjson_is_arr(v) ? v : NULL;
}

// replaces the value in place, so the order of keys written by another tool survives; new keys go to the end
void sh_json_set_val(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key, yyjson_mut_val *value) {
    yyjson_mut_obj_put(obj, yyjson_mut_strcpy(d, key), value);
}

void sh_json_set_str(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key, const char *value) {
    sh_json_set_val(d, obj, key, yyjson_mut_strcpy(d, value ? value : ""));
}

void sh_json_set_int(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key, long long value) {
    sh_json_set_val(d, obj, key, yyjson_mut_sint(d, value));
}

void sh_json_set_bool(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key, bool value) {
    sh_json_set_val(d, obj, key, yyjson_mut_bool(d, value));
}

yyjson_mut_val *sh_json_get_or_add_obj(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key) {
    yyjson_mut_val *v = yyjson_mut_obj_get(obj, key);
    if (yyjson_mut_is_obj(v)) return v;
    v = yyjson_mut_obj(d);
    sh_json_set_val(d, obj, key, v);
    return v;
}

const char *sh_json_mut_str(yyjson_mut_val *obj, const char *key) {
    const char *s = yyjson_mut_get_str(yyjson_mut_obj_get(obj, key));
    return s ? s : "";
}

// ---- lookup tables ----

typedef struct {
    const char *key;
    size_t len;
    size_t pos;  // order in the object: the first of repeated keys wins
    yyjson_mut_val *val;
} member;

struct sh_json_index {
    member *m;
    size_t n;
};

static int cmp_key(const char *a, size_t alen, const char *b, size_t blen) {
    const int c = memcmp(a, b, alen < blen ? alen : blen);
    return c ? c : (alen < blen ? -1 : alen > blen);
}

static int cmp_member(const void *x, const void *y) {
    const member *a = x, *b = y;
    const int c = cmp_key(a->key, a->len, b->key, b->len);
    return c ? c : (a->pos < b->pos ? -1 : a->pos > b->pos);
}

sh_json_index *sh_json_index_new(yyjson_mut_val *obj) {
    sh_json_index *ix = sh_xcalloc(1, sizeof *ix);
    const size_t n = yyjson_mut_is_obj(obj) ? yyjson_mut_obj_size(obj) : 0;
    ix->m = sh_xcalloc(n + 1, sizeof *ix->m);
    if (n == 0) return ix;
    size_t i, max;
    yyjson_mut_val *key, *val;
    yyjson_mut_obj_foreach(obj, i, max, key, val) {
        ix->m[ix->n] = (member){yyjson_mut_get_str(key), yyjson_mut_get_len(key), ix->n, val};
        ++ix->n;
    }
    qsort(ix->m, ix->n, sizeof *ix->m, cmp_member);
    return ix;
}

yyjson_mut_val *sh_json_index_get(const sh_json_index *ix, const char *key) {
    const size_t len = strlen(key);
    size_t lo = 0, hi = ix->n;
    while (lo < hi) {  // the first member not below key
        const size_t mid = lo + (hi - lo) / 2;
        if (cmp_key(ix->m[mid].key, ix->m[mid].len, key, len) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo < ix->n && cmp_key(ix->m[lo].key, ix->m[lo].len, key, len) == 0 ? ix->m[lo].val : NULL;
}

void sh_json_index_free(sh_json_index *ix) {
    if (!ix) return;
    free(ix->m);
    free(ix);
}
