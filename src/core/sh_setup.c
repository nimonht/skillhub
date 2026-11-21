#include "sh_setup.h"

#include <stdlib.h>
#include <string.h>

#include "sh_fs.h"
#include "sh_str.h"

#define MAX_FILE (1024 * 1024)  // a file larger than this is not read for ${CLAUDE_PLUGIN_ROOT}
#define MAX_WALK 20000          // files looked at in one skill: more than this is no skill
#define MAX_NOTES 64
#define MAX_DETAIL 120

struct sh_setup_hint {
    char *folder;
    const char *manifest;  // the manifest it is about
    const char *command;
};

// A manifest and what installs what it lists. A lock file next to it may name another tool (LOCKS).
static const struct {
    const char *name, *command;
} MANIFESTS[] = {
    {"package.json", "npm install"}, {"requirements.txt", "pip install -r requirements.txt"}, {"pyproject.toml", "pip install ."},
    {"Pipfile", "pipenv install"},    {"Gemfile", "bundle install"},                           {"composer.json", "composer install"},
};

static const struct {
    const char *name, *manifest, *command;
} LOCKS[] = {
    {"pnpm-lock.yaml", "package.json", "pnpm install"}, {"yarn.lock", "package.json", "yarn install"},
    {"bun.lock", "package.json", "bun install"},        {"bun.lockb", "package.json", "bun install"},
    {"uv.lock", "pyproject.toml", "uv sync"},           {"poetry.lock", "pyproject.toml", "poetry install"},
};

// what an author ships to install a skill by hand: read, never run
static const char *const SCRIPTS[] = {"install.sh", "install.ps1", "install.py", "install.bat", "install.cmd", "setup.sh", "setup.ps1", "bootstrap.sh"};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

// folders whose files are someone else's: installed packages, never the skill's own needs
static bool vendored(const char *rel) {
    static const char *const dirs[] = {"node_modules", ".venv", "venv", "__pycache__", ".git"};
    for (size_t i = 0; i < COUNT(dirs); ++i) {
        const size_t n = strlen(dirs[i]);
        for (const char *p = rel; (p = strstr(p, dirs[i])) != NULL; p += n)
            if ((p == rel || p[-1] == '/') && p[n] == '/') return true;
    }
    return false;
}

// the folder of a path inside the skill, "" for a file at its top
static char *folder_of(const char *rel) {
    const char *slash = strrchr(rel, '/');
    return slash ? sh_strndup(rel, (size_t)(slash - rel)) : sh_strdup("");
}

static bool same_ascii_ci(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) {
        const char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return false;
    }
    return *a == *b;
}

static bool contains(const char *text, size_t n, const char *needle) {
    const size_t k = strlen(needle);
    for (size_t i = 0; i + k <= n; ++i)
        if (memcmp(text + i, needle, k) == 0) return true;
    return false;
}

static void push(sh_setup *s, sh_setup_kind kind, const char *rel, const char *command, const char *detail) {
    if (s->len >= MAX_NOTES) return;
    if (s->len == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 4;
        s->items = sh_xrealloc(s->items, s->cap * sizeof *s->items);
    }
    sh_setup_note *n = &s->items[s->len++];
    n->kind = kind;
    n->file = sh_strdup(rel);
    n->folder = folder_of(rel);
    n->command = command;
    n->detail = sh_strdup(detail ? detail : "");
}

static const char *base_name(const char *rel) {
    const char *slash = strrchr(rel, '/');
    return slash ? slash + 1 : rel;
}

void sh_setup_file(sh_setup *s, const char *rel) {
    if (vendored(rel)) return;
    const char *base = base_name(rel);
    for (size_t i = 0; i < COUNT(MANIFESTS); ++i) {
        if (strcmp(base, MANIFESTS[i].name) != 0) continue;
        push(s, SH_SETUP_DEPENDENCIES, rel, MANIFESTS[i].command, NULL);
        return;
    }
    for (size_t i = 0; i < COUNT(LOCKS); ++i) {
        if (strcmp(base, LOCKS[i].name) != 0) continue;
        if (s->n_hints == s->cap_hints) {
            s->cap_hints = s->cap_hints ? s->cap_hints * 2 : 4;
            s->hints = sh_xrealloc(s->hints, s->cap_hints * sizeof *s->hints);
        }
        char *folder = folder_of(rel);
        s->hints[s->n_hints++] = (struct sh_setup_hint){folder, LOCKS[i].manifest, LOCKS[i].command};
        return;
    }
    for (size_t i = 0; i < COUNT(SCRIPTS); ++i) {
        if (!same_ascii_ci(base, SCRIPTS[i])) continue;
        push(s, SH_SETUP_SCRIPT, rel, NULL, NULL);
        return;
    }
}

static bool path_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/';
}

void sh_setup_text(sh_setup *s, const char *rel, const char *text, size_t n) {
    static const char var[] = "CLAUDE_PLUGIN_ROOT";
    const size_t vn = sizeof var - 1;
    if (vendored(rel)) return;
    const char *mention = NULL, *path = NULL;
    size_t path_len = 0;
    for (size_t i = 0; i + vn <= n && !path; ++i) {
        if (memcmp(text + i, var, vn) != 0) continue;
        if (!mention) mention = text + i;
        size_t j = i + vn;
        if (j < n && text[j] == '}') ++j;
        if (j < n && text[j] == '/') {  // a path inside the plugin: say which one
            const size_t from = ++j;
            while (j < n && j - from < MAX_DETAIL && path_char(text[j])) ++j;
            if (j > from) {
                path = text + from;
                path_len = j - from;
            }
        }
        i = j > i ? j - 1 : i;
    }
    if (!mention) return;
    char *detail = path ? sh_strndup(path, path_len) : sh_strdup("");
    push(s, SH_SETUP_PLUGIN_ROOT, rel, NULL, detail);
    free(detail);
}

void sh_setup_plugin_root(sh_setup *s, const char *rel, const char *detail) {
    if (!vendored(rel)) push(s, SH_SETUP_PLUGIN_ROOT, rel, NULL, detail);
}

static int cmp_note(const void *a, const void *b) {
    const sh_setup_note *x = a, *y = b;
    if (x->kind != y->kind) return x->kind < y->kind ? -1 : 1;
    return strcmp(x->file, y->file);
}

void sh_setup_finish(sh_setup *s) {
    for (size_t h = 0; h < s->n_hints; ++h) {
        for (size_t i = 0; i < s->len; ++i) {
            sh_setup_note *n = &s->items[i];
            if (n->kind == SH_SETUP_DEPENDENCIES && strcmp(n->folder, s->hints[h].folder) == 0 && strcmp(base_name(n->file), s->hints[h].manifest) == 0)
                n->command = s->hints[h].command;
        }
        free(s->hints[h].folder);
    }
    free(s->hints);
    s->hints = NULL;
    s->n_hints = s->cap_hints = 0;
    if (s->len > 1) qsort(s->items, s->len, sizeof *s->items, cmp_note);
}

// a manifest that lists something to install; anything unreadable counts, to be safe
static bool lists_something(const char *base, const char *text, size_t n) {
    if (strcmp(base, "package.json") == 0) {
        static const char *const keys[] = {"\"dependencies\"", "\"devDependencies\"", "\"optionalDependencies\"", "\"peerDependencies\""};
        for (size_t i = 0; i < COUNT(keys); ++i)
            if (contains(text, n, keys[i])) return true;
        return false;
    }
    if (strcmp(base, "requirements.txt") == 0) {  // a line that is not blank and not a comment
        for (size_t i = 0; i < n;) {
            size_t j = i;
            while (j < n && (text[j] == ' ' || text[j] == '\t')) ++j;
            if (j < n && text[j] != '\n' && text[j] != '\r' && text[j] != '#') return true;
            while (i < n && text[i] != '\n') ++i;
            ++i;
        }
        return false;
    }
    return true;
}

typedef struct {
    sh_setup *out;
    size_t seen;
} setup_walk;

static bool walk_one(const char *rel, const char *full, bool is_link, void *user) {
    setup_walk *w = user;
    if (++w->seen > MAX_WALK) return false;
    if (is_link || vendored(rel)) return true;
    const size_t before = w->out->len;
    sh_setup_file(w->out, rel);
    const int64_t size = sh_file_size(full);
    if (size < 0 || size > MAX_FILE) return true;
    sh_buf b = SH_BUF_INIT;
    if (sh_read_file(full, MAX_FILE, &b)) {
        const char *text = b.data ? b.data : "";
        if (!memchr(text, '\0', b.len < 8192 ? b.len : 8192)) sh_setup_text(w->out, rel, text, b.len);  // text, not a binary
        if (w->out->len > before && w->out->items[before].kind == SH_SETUP_DEPENDENCIES && !lists_something(base_name(rel), text, b.len)) {
            sh_setup_note *n = &w->out->items[before];  // nothing to install: no note
            free(n->file);
            free(n->folder);
            free(n->detail);
            memmove(n, n + 1, (w->out->len - before - 1) * sizeof *n);
            --w->out->len;
        }
    }
    sh_buf_free(&b);
    return true;
}

void sh_setup_dir(const char *dir, sh_setup *out) {
    memset(out, 0, sizeof *out);
    setup_walk w = {out, 0};
    sh_walk_files(dir, walk_one, &w);
    sh_setup_finish(out);
}

void sh_setup_free(sh_setup *s) {
    for (size_t i = 0; i < s->len; ++i) {
        free(s->items[i].file);
        free(s->items[i].folder);
        free(s->items[i].detail);
    }
    free(s->items);
    for (size_t h = 0; h < s->n_hints; ++h) free(s->hints[h].folder);
    free(s->hints);
    memset(s, 0, sizeof *s);
}

const char *sh_setup_kind_name(sh_setup_kind k) {
    switch (k) {
    case SH_SETUP_DEPENDENCIES: return "dependencies";
    case SH_SETUP_SCRIPT: return "script";
    case SH_SETUP_PLUGIN_ROOT: return "pluginRoot";
    }
    return "";
}

void sh_setup_json(yyjson_mut_doc *d, yyjson_mut_val *list, const sh_setup *s) {
    for (size_t i = 0; i < s->len; ++i) {
        const sh_setup_note *n = &s->items[i];
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, o, "kind", sh_setup_kind_name(n->kind));
        yyjson_mut_obj_add_strcpy(d, o, "file", n->file);
        yyjson_mut_obj_add_strcpy(d, o, "folder", n->folder);
        if (n->command) yyjson_mut_obj_add_str(d, o, "command", n->command);
        if (*n->detail) yyjson_mut_obj_add_strcpy(d, o, "detail", n->detail);
        yyjson_mut_arr_append(list, o);
    }
}
