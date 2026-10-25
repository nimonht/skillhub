#include "sh_frontmatter.h"

#include <stdlib.h>
#include <string.h>

#include "sh_fs.h"
#include "sh_str.h"
#include "sh_utf.h"

static void put(sh_frontmatter *fm, char *key, char *value) {
    for (size_t i = 0; i < fm->len; ++i) {  // a repeated key keeps the last value, like QMap::insert
        if (strcmp(fm->keys[i], key) == 0) {
            free(fm->values[i]);
            fm->values[i] = value;
            free(key);
            return;
        }
    }
    fm->keys = sh_xrealloc(fm->keys, (fm->len + 1) * sizeof *fm->keys);
    fm->values = sh_xrealloc(fm->values, (fm->len + 1) * sizeof *fm->values);
    fm->keys[fm->len] = key;
    fm->values[fm->len] = value;
    ++fm->len;
}

static bool first_is_space(const char *line) {
    const char *p = line;
    return *p && sh_uni_is_space(sh_utf8_next(&p, line + strlen(line)));
}

static bool trimmed_is(const char *line, const char *want) {
    char *t = sh_trimmed(line, strlen(line));
    const bool same = strcmp(t, want) == 0;
    free(t);
    return same;
}

void sh_fm_parse(const char *raw, size_t n, sh_frontmatter *fm) {
    memset(fm, 0, sizeof *fm);
    if (n >= 3 && memcmp(raw, "\xEF\xBB\xBF", 3) == 0) {  // Qt's UTF-8 decoder dropped a leading byte order mark
        raw += 3;
        n -= 3;
    }
    char *clean = sh_utf8_clean(raw, n);
    // "\r\n" becomes "\n" first, as in 1.x; a lone "\r" stays part of the line
    sh_buf text = SH_BUF_INIT;
    for (const char *p = clean; *p; ++p) {
        if (p[0] == '\r' && p[1] == '\n') continue;
        sh_buf_putc(&text, *p);
    }
    free(clean);
    sh_strv lines = SH_STRV_INIT;
    sh_split(sh_buf_str(&text), text.len, '\n', false, &lines);
    sh_buf_free(&text);

    size_t end = 0;
    if (lines.len > 0 && trimmed_is(lines.items[0], "---")) {
        for (size_t i = 1; i < lines.len; ++i) {
            if (trimmed_is(lines.items[i], "---")) {
                end = i;
                break;
            }
        }
    }
    for (size_t i = 1; i < end; ++i) {
        const char *ln = lines.items[i];
        if (!*ln || first_is_space(ln) || ln[0] == '#') continue;
        const char *colon = strchr(ln, ':');
        if (!colon || colon == ln) continue;
        char *key = sh_trimmed(ln, (size_t)(colon - ln));
        char *val = sh_trimmed(colon + 1, strlen(colon + 1));
        static const char *const block[] = {"|", ">", ">-", "|-", ">+", "|+"};
        bool is_block = false;
        for (size_t k = 0; k < sizeof block / sizeof block[0]; ++k)
            if (strcmp(val, block[k]) == 0) is_block = true;
        const size_t vlen = strlen(val);
        if (is_block) {
            sh_buf parts = SH_BUF_INIT;
            bool first = true;
            while (i + 1 < end && (!*lines.items[i + 1] || first_is_space(lines.items[i + 1]))) {
                char *part = sh_trimmed(lines.items[i + 1], strlen(lines.items[i + 1]));
                if (!first) sh_buf_putc(&parts, ' ');
                sh_buf_puts(&parts, part);
                free(part);
                first = false;
                ++i;
            }
            free(val);
            val = sh_simplified(sh_buf_str(&parts), parts.len);
            sh_buf_free(&parts);
        } else if (vlen >= 2 && ((val[0] == '"' && val[vlen - 1] == '"') || (val[0] == '\'' && val[vlen - 1] == '\''))) {
            char *inner = sh_strndup(val + 1, vlen - 2);
            free(val);
            val = inner;
        } else if (vlen == 0) {
            while (i + 1 < end && lines.items[i + 1][0] == ' ') ++i;  // a nested map: skipped
        }
        put(fm, key, val);
    }
    sh_strv_free(&lines);
}

void sh_fm_read(const char *skill_md, sh_frontmatter *fm) {
    sh_buf b = SH_BUF_INIT;
    if (sh_read_file(skill_md, 64 * 1024, &b)) {
        sh_fm_parse(sh_buf_str(&b), b.len, fm);
    } else {
        memset(fm, 0, sizeof *fm);
    }
    sh_buf_free(&b);
}

const char *sh_fm_get(const sh_frontmatter *fm, const char *key) {
    for (size_t i = 0; i < fm->len; ++i)
        if (strcmp(fm->keys[i], key) == 0) return fm->values[i];
    return NULL;
}

const char *sh_fm_value(const sh_frontmatter *fm, const char *key) {
    const char *v = sh_fm_get(fm, key);
    return v ? v : "";
}

void sh_fm_free(sh_frontmatter *fm) {
    for (size_t i = 0; i < fm->len; ++i) {
        free(fm->keys[i]);
        free(fm->values[i]);
    }
    free(fm->keys);
    free(fm->values);
    memset(fm, 0, sizeof *fm);
}
