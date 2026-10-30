// The SHA-256 folder hash of docs/formats.md section 3 (`computedHash`), and the file order it depends on: JavaScript's
// localeCompare with no locale, which is ICU's root collation. Paths compare by base letters first (punctuation, then
// digits, then letters, case ignored), then by accents, then by case (lower before upper), as ICU does. ASCII follows
// ICU exactly; the Latin letters with accents that skill authors use are folded to their base letter; other scripts sort
// after Latin, by code point. Checked against vectors from real npx (tests/vectors/computed-hash.json).
#include "sh_collate.h"

#include <stdlib.h>
#include <string.h>

#include "sh_fs.h"
#include "sh_hash.h"
#include "sh_path.h"
#include "sh_utf.h"

// ICU root order of ASCII punctuation and symbols (DUCET, variable weighting non-ignorable)
static const char PUNCT[] = "\t\n\v\f\r _-,;:!?.'\"()[]{}@*/\\&#%`^+<=>|~$";

typedef struct {
    unsigned primary;  // 0 ends the key
    unsigned char secondary, tertiary;
} weight;

// Latin-1 and Latin Extended-A letters with accents: base letter (lower case), and how far the accent sorts
static bool fold_latin(uint32_t cp, char *base, unsigned char *accent, bool *upper) {
    static const struct { uint32_t cp; char base; unsigned char accent; } map[] = {
        {0xE0, 'a', 1}, {0xE1, 'a', 2}, {0xE2, 'a', 3}, {0xE3, 'a', 4}, {0xE4, 'a', 5}, {0xE5, 'a', 6},
        {0xE7, 'c', 1}, {0xE8, 'e', 1}, {0xE9, 'e', 2}, {0xEA, 'e', 3}, {0xEB, 'e', 5}, {0xEC, 'i', 1},
        {0xED, 'i', 2}, {0xEE, 'i', 3}, {0xEF, 'i', 5}, {0xF1, 'n', 4}, {0xF2, 'o', 1}, {0xF3, 'o', 2},
        {0xF4, 'o', 3}, {0xF5, 'o', 4}, {0xF6, 'o', 5}, {0xF8, 'o', 7}, {0xF9, 'u', 1}, {0xFA, 'u', 2},
        {0xFB, 'u', 3}, {0xFC, 'u', 5}, {0xFD, 'y', 2}, {0xFF, 'y', 5},
    };
    uint32_t lower = cp;
    *upper = false;
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) {
        lower = cp + 0x20;
        *upper = true;
    }
    for (size_t i = 0; i < sizeof map / sizeof map[0]; ++i) {
        if (map[i].cp == lower) {
            *base = map[i].base;
            *accent = map[i].accent;
            return true;
        }
    }
    return false;
}

// the collation elements of a path; ß expands to two s (with a case difference from "ss")
static weight *key_of(const char *s, size_t *n_out) {
    const size_t len = strlen(s);
    weight *k = sh_xcalloc(2 * len + 2, sizeof *k);
    size_t n = 0;
    const char *p = s, *end = s + len;
    while (p < end) {
        const uint32_t cp = sh_utf8_next(&p, end);
        char base;
        unsigned char accent;
        bool upper;
        const char *in_punct = cp < 128 && cp ? strchr(PUNCT, (int)cp) : NULL;
        if (in_punct) k[n++] = (weight){1 + (unsigned)(in_punct - PUNCT), 0, 0};
        else if (cp >= '0' && cp <= '9') k[n++] = (weight){100 + (cp - '0'), 0, 0};
        else if (cp >= 'a' && cp <= 'z') k[n++] = (weight){200 + (cp - 'a'), 0, 0};
        else if (cp >= 'A' && cp <= 'Z') k[n++] = (weight){200 + (cp - 'A'), 0, 1};
        else if (cp == 0xDF) {  // ß: like "ss", one step further at the third level
            k[n++] = (weight){200 + ('s' - 'a'), 0, 2};
            k[n++] = (weight){200 + ('s' - 'a'), 0, 2};
        } else if (fold_latin(cp, &base, &accent, &upper)) {
            k[n++] = (weight){200 + (unsigned)(base - 'a'), accent, upper};
        } else {
            k[n++] = (weight){1000 + cp, 0, 0};  // other scripts after Latin (Greek before CJK, as by code point)
        }
    }
    k[n] = (weight){0, 0, 0};
    *n_out = n;
    return k;
}

static int compare_level(const weight *a, size_t na, const weight *b, size_t nb, int level) {
    for (size_t i = 0; i < na && i < nb; ++i) {
        const unsigned x = level == 1 ? a[i].primary : level == 2 ? a[i].secondary : a[i].tertiary;
        const unsigned y = level == 1 ? b[i].primary : level == 2 ? b[i].secondary : b[i].tertiary;
        if (x != y) return x < y ? -1 : 1;
    }
    return na == nb ? 0 : na < nb ? -1 : 1;
}

int sh_locale_compare(const char *a, const char *b) {
    size_t na, nb;
    weight *ka = key_of(a, &na), *kb = key_of(b, &nb);
    int c = 0;
    for (int level = 1; level <= 3 && c == 0; ++level) c = compare_level(ka, na, kb, nb, level);
    free(ka);
    free(kb);
    return c ? c : strcmp(a, b);  // identical by collation: a stable, total order still
}

// ---- the folder hash ----

bool sh_folder_hash_skips(const char *rel) {
    sh_strv seg = SH_STRV_INIT;
    sh_split(rel, strlen(rel), '/', true, &seg);
    bool skip = false;
    for (size_t i = 0; i + 1 < seg.len && !skip; ++i) skip = strcmp(seg.items[i], ".git") == 0 || strcmp(seg.items[i], "node_modules") == 0;
    sh_strv_free(&seg);
    return skip;
}

static bool collect(const char *rel, const char *full, bool is_link, void *user) {
    (void)full;
    if (!is_link && !sh_folder_hash_skips(rel)) sh_strv_push(user, rel);  // regular files only: links are not hashed
    return true;
}

char *sh_folder_hash(const char *dir) {
    sh_strv files = SH_STRV_INIT;
    sh_walk_files(dir, collect, &files);
    sh_strv_sort(&files, sh_locale_compare);
    sh_sha256 h;
    sh_sha256_init(&h);
    for (size_t i = 0; i < files.len; ++i) {
        sh_sha256_update(&h, files.items[i], strlen(files.items[i]));
        char *full = sh_path_join(dir, files.items[i]);
        sh_buf b = SH_BUF_INIT;
        if (sh_read_file(full, 0, &b)) sh_sha256_update(&h, b.data ? b.data : "", b.len);
        sh_buf_free(&b);
        free(full);
    }
    sh_strv_free(&files);
    unsigned char d[32];
    char *hex = sh_xmalloc(65);
    sh_sha256_final(&h, d);
    sh_hex(d, 32, hex);
    return hex;
}
