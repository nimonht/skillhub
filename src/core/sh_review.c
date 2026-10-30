#include "sh_review.h"

#include <stdlib.h>
#include <string.h>

#include "sh_fs.h"
#include "sh_git.h"
#include "sh_path.h"
#include "sh_str.h"
#include "sh_utf.h"

#ifndef _WIN32
#include <strings.h>
#else
#define strcasecmp _stricmp
#endif

#define MAX_FILE_PATCH (256 * 1024)
#define MAX_PATCHES (4 * 1024 * 1024)

void sh_review_free(sh_review *r) {
    for (size_t i = 0; i < r->n; ++i) {
        free(r->files[i].path);
        free(r->files[i].patch);
    }
    free(r->files);
    free(r->name);
    free(r->stage);
    free(r->tree);
    free(r->err);
    memset(r, 0, sizeof *r);
}

bool sh_is_risky_file(const char *rel, const char *full) {
    static const char *const scripts[] = {".sh", ".bash", ".zsh", ".fish", ".py", ".rb", ".pl", ".js", ".mjs", ".cjs", ".ts", ".ps1", ".psm1", ".bat", ".cmd", ".exe", ".dll", ".so", ".dylib"};
    const char *dot = strrchr(rel, '.');
    for (size_t i = 0; dot && i < sizeof scripts / sizeof scripts[0]; ++i)
        if (strcasecmp(dot, scripts[i]) == 0) return true;
    if (sh_is_executable(full)) return true;
    sh_buf b = SH_BUF_INIT;
    const bool shebang = sh_read_file(full, 2, &b) && b.len >= 2 && b.data[0] == '#' && b.data[1] == '!';
    sh_buf_free(&b);
    return shebang;
}

typedef struct {
    const char *installed;
    bool found;
} scripts_walk;

static bool same_content(const char *a, const char *b) {
    sh_buf x = SH_BUF_INIT, y = SH_BUF_INIT;
    const bool same = sh_read_file(a, 0, &x) && sh_read_file(b, 0, &y) && x.len == y.len && (x.len == 0 || memcmp(x.data, y.data, x.len) == 0);
    sh_buf_free(&x);
    sh_buf_free(&y);
    return same;
}

static bool scripts_one(const char *rel, const char *full, bool is_link, void *user) {
    scripts_walk *w = user;
    if (is_link || !sh_is_risky_file(rel, full)) return true;
    char *other = sh_path_join(w->installed, rel);
    w->found = !same_content(full, other);
    free(other);
    return !w->found;
}

bool sh_adds_scripts(const char *stage, const char *installed) {
    scripts_walk w = {installed, false};
    sh_walk_files(stage, scripts_one, &w);
    return w.found;
}

// ---- the file list ----

static char *clean(const char *s) { return sh_utf8_clean(s, strlen(s)); }  // files and their names may be in any encoding

// one part of git's output up to its NUL
static const char *next_part(const char **p, const char *end) {
    if (*p >= end) return NULL;
    const char *part = *p;
    const char *nul = memchr(part, '\0', (size_t)(end - part));
    *p = nul ? nul + 1 : end;
    return part;
}

static const char *strip_top(const char *path) {  // "old/a/b" -> "a/b"
    const char *slash = strchr(path, '/');
    return slash ? slash + 1 : path;
}

static void add_file(sh_review *r, size_t *cap, const char *stage, const char *counts, const char *from, const char *to, bool exec) {
    if (r->n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        r->files = sh_xrealloc(r->files, *cap * sizeof *r->files);
    }
    sh_diff_file *f = &r->files[r->n++];
    memset(f, 0, sizeof *f);
    const bool added = strcmp(from, "/dev/null") == 0, gone = strcmp(to, "/dev/null") == 0;
    f->path = clean(strip_top(added ? to : from));
    f->status = added ? 'A' : gone ? 'D' : 'M';
    f->binary = counts[0] == '-';
    if (!f->binary) {
        f->added = atoi(counts);
        const char *tab = strchr(counts, '\t');
        f->removed = tab ? atoi(tab + 1) : 0;
    }
    f->exec = exec;
    f->patch = sh_strdup("");
    if (!gone) {
        char *full = sh_path_join(stage, to);
        f->risk = sh_is_risky_file(to, full);
        free(full);
        r->risk = r->risk || f->risk;
    }
}

// `--raw --numstat -z`: one raw record per file, then one numstat record per file, in the same order
static void read_list(sh_review *r, const char *stage, const char *out, size_t len) {
    const char *p = out, *end = out + len;
    sh_strv modes = SH_STRV_INIT;
    size_t cap = 0, i = 0;
    const char *part;
    while ((part = next_part(&p, end))) {
        if (part[0] == ':') {
            sh_strv_push(&modes, part);
            next_part(&p, end);  // its path; the numstat record has both
            continue;
        }
        const char *from = next_part(&p, end), *to = next_part(&p, end);
        if (!from || !to) break;
        const bool exec = i < modes.len && strlen(modes.items[i]) > 14 && strncmp(modes.items[i] + 8, "100755", 6) == 0;
        add_file(r, &cap, stage, part, from, to, exec);
        ++i;
    }
    sh_strv_free(&modes);
}

// ---- the patches ----

// the hunks of one `diff --git` block, up to what may still be shown
static void set_patch(sh_review *r, sh_diff_file *f, const char *block, size_t n, size_t *total) {
    const char *hunks = NULL;
    for (const char *line = block; line < block + n;) {
        if (strncmp(line, "@@ ", 3) == 0) {
            hunks = line;
            break;
        }
        const char *nl = memchr(line, '\n', (size_t)(block + n - line));
        line = nl ? nl + 1 : block + n;
    }
    if (!hunks) return;
    size_t len = (size_t)(block + n - hunks);
    const size_t room = *total >= MAX_PATCHES ? 0 : MAX_PATCHES - *total;
    const size_t limit = room < MAX_FILE_PATCH ? room : MAX_FILE_PATCH;
    if (len > limit) {
        len = limit;
        while (len > 0 && hunks[len - 1] != '\n') --len;  // whole lines only
        f->cut = true;
        r->truncated = true;
    }
    *total += len;
    char *raw = sh_strndup(hunks, len);
    free(f->patch);
    f->patch = clean(raw);
    free(raw);
}

static void read_patches(sh_review *r, const char *out, size_t len) {
    static const char marker[] = "diff --git ";
    const size_t mn = sizeof marker - 1;
    const char *end = out + len;
    size_t i = 0, total = 0;
    const char *block = len >= mn && strncmp(out, marker, mn) == 0 ? out : NULL;
    while (block && i < r->n) {
        const char *next = NULL;
        for (const char *nl = memchr(block, '\n', (size_t)(end - block)); nl && !next; nl = memchr(nl + 1, '\n', (size_t)(end - nl - 1))) {
            if ((size_t)(end - nl - 1) > mn && strncmp(nl + 1, marker, mn) == 0) next = nl + 1;
        }
        set_patch(r, &r->files[i++], block, (size_t)((next ? next : end) - block), &total);
        block = next;
    }
}

void sh_review_compare(const char *stage, sh_review *r) {
    // git exits 1 when the folders differ, so only an error message says it failed
    static const char *const list[] = {"-c", "core.quotepath=false", "diff", "--no-index", "--no-color", "--no-ext-diff", "--no-textconv", "--no-renames", "--raw", "--numstat", "-z", "--", "old", "new", NULL};
    static const char *const patches[] = {"-c", "core.quotepath=false", "diff", "--no-index", "--no-color", "--no-ext-diff", "--no-textconv", "--no-renames", "-p", "--", "old", "new", NULL};
    sh_git_result g = sh_git_run(list, stage, 0);
    if (!g.ok && !g.out.len) {
        r->err = sh_strdup(g.err && *g.err ? g.err : "git diff failed");
    } else {
        read_list(r, stage, sh_buf_str(&g.out), g.out.len);
        sh_git_result p = sh_git_run(patches, stage, 0);
        read_patches(r, sh_buf_str(&p.out), p.out.len);
        sh_git_result_free(&p);
    }
    sh_git_result_free(&g);
}
