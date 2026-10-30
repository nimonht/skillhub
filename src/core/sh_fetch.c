#include "sh_fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh_fs.h"
#include "sh_hash.h"
#include "sh_json.h"
#include "sh_palette.h"
#include "sh_path.h"
#include "sh_state.h"
#include "sh_sysinfo.h"
#include "sh_time.h"
#include "sh_utf.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "stb_image.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define LOGO_W 10
#define LOGO_H 6
#define GAP 4
#define SIZES_MAX_AGE_MS (60LL * 60 * 1000)

typedef struct {
    sh_sysinfo sys;
    int skills, tracked, local, agents, known, problems, stray;
    int updates;       // -1: never checked
    char *checked;     // "2 h ago"
    char *folder;
    long long folder_bytes, backup_bytes, cache_bytes, disk_total, disk_free;
} facts;

static char *shown_bytes(long long n) {
    if (n >= 1024LL * 1024 * 1024 * 10) return sh_strfmt("%lld GB", n / (1024LL * 1024 * 1024));
    if (n >= 1024LL * 1024 * 1024) return sh_strfmt("%.1f GB", (double)n / (1024.0 * 1024 * 1024));
    if (n >= 1024 * 1024) return sh_strfmt("%lld MB", n / (1024 * 1024));
    return sh_strfmt("%lld KB", n / 1024);
}

// Folder sizes are remembered for an hour, or until the folder changes: a banner has no time to walk ten thousand skills.
static long long cached_size(yyjson_mut_doc *cache, const char *dir, bool *changed) {
    yyjson_mut_val *root = yyjson_mut_doc_get_root(cache);
    yyjson_mut_val *row = yyjson_mut_obj_get(root, dir);
    const long long stamp = sh_mtime_ms(dir), now = sh_now_ms();
    if (yyjson_mut_is_obj(row) && yyjson_mut_get_sint(yyjson_mut_obj_get(row, "stamp")) == stamp && now - yyjson_mut_get_sint(yyjson_mut_obj_get(row, "at")) < SIZES_MAX_AGE_MS)
        return yyjson_mut_get_sint(yyjson_mut_obj_get(row, "bytes"));
    const long long bytes = sh_tree_bytes(dir);
    yyjson_mut_val *fresh = yyjson_mut_obj(cache);
    yyjson_mut_obj_add_sint(cache, fresh, "stamp", stamp);
    yyjson_mut_obj_add_sint(cache, fresh, "at", now);
    yyjson_mut_obj_add_sint(cache, fresh, "bytes", bytes);
    sh_json_set_val(cache, root, dir, fresh);
    *changed = true;
    return bytes;
}

static void read_sizes(const sh_store *st, facts *f) {
    char *path = sh_path_join(st->cache, "fetch-sizes.json");
    yyjson_doc *doc = NULL;
    yyjson_mut_doc *m = sh_json_read(path, 0, &doc) == SH_JSON_OK ? yyjson_doc_mut_copy(doc, NULL) : yyjson_mut_doc_new(NULL);
    yyjson_doc_free(doc);
    if (!yyjson_mut_is_obj(yyjson_mut_doc_get_root(m))) yyjson_mut_doc_set_root(m, yyjson_mut_obj(m));
    bool changed = false;
    f->folder_bytes = cached_size(m, st->central, &changed);
    f->backup_bytes = cached_size(m, st->backup, &changed);
    f->cache_bytes = cached_size(m, st->cache, &changed);
    if (changed) sh_json_write(path, m, false, true);
    yyjson_mut_doc_free(m);
    free(path);
}

static void read_check(const sh_store *st, facts *f) {
    f->updates = -1;
    f->checked = sh_strdup("");
    char *path = sh_path_join(st->cache, "sources.json");
    yyjson_doc *doc = NULL;
    if (sh_json_read(path, 0, &doc) == SH_JSON_OK) {
        yyjson_val *last = yyjson_obj_get(yyjson_doc_get_root(doc), "lastCheck");
        yyjson_val *mine = last ? yyjson_obj_get(last, st->scope) : NULL;
        if (mine) {
            f->updates = (int)sh_json_int(mine, "available", 0);
            const long long ago = (sh_now_ms() - sh_json_int(mine, "atMs", 0)) / 1000;
            free(f->checked);
            f->checked = ago < 60 ? sh_strdup("just now") : ago < 3600 ? sh_strfmt("%lld min ago", ago / 60) : ago < 86400 ? sh_strfmt("%lld h ago", ago / 3600) : sh_strfmt("%lld d ago", ago / 86400);
        }
        yyjson_doc_free(doc);
    }
    free(path);
}

static void gather(const sh_store *st, facts *f) {
    memset(f, 0, sizeof *f);
    sh_sysinfo_read(&f->sys);
    sh_skills sk;
    sh_store_scan(st, &sk);
    f->skills = (int)sk.len;
    for (size_t i = 0; i < sk.len; ++i) {
        if (sh_source_tracked(&sk.items[i].src)) ++f->tracked;
        else ++f->local;
        for (size_t k = 0; k < sk.items[i].n_links; ++k) {
            const sh_link_state l = sk.items[i].links[k];
            f->problems += l == SH_LINK_MISSING || l == SH_LINK_COPY || l == SH_LINK_FOREIGN;
        }
    }
    sh_skills_free(&sk);
    f->agents = (int)st->n_agents;
    f->known = (int)st->n_providers;
    read_check(st, f);
    read_sizes(st, f);
    f->folder = sh_tilde(st->central);
    int64_t total = 0, free_bytes = 0;
    if (sh_disk_usage(sh_is_dir(st->central) ? st->central : st->home, &total, &free_bytes)) {
        f->disk_total = total;
        f->disk_free = free_bytes;
    }
}

static void facts_free(facts *f) {
    sh_sysinfo_free(&f->sys);
    free(f->checked);
    free(f->folder);
}

static char *updates_text(const facts *f) {
    if (f->updates < 0) return sh_strdup("not checked yet");
    return sh_strfmt("%d available · checked %s", f->updates, f->checked);
}

// ---- colour ----

typedef struct {
    sh_fetch_color mode;
    int r, g, b;  // the accent
} ink;

static void paint(sh_buf *b, const ink *c, int r, int g, int bl) {
    if (c->mode == SH_COLOR_TRUE) {
        sh_buf_printf(b, "\x1b[38;2;%d;%d;%dm", r, g, bl);
    } else if (c->mode == SH_COLOR_256) {
        sh_buf_printf(b, "\x1b[38;5;%dm", 16 + 36 * (r * 5 / 255) + 6 * (g * 5 / 255) + bl * 5 / 255);
    }
}

static void plain(sh_buf *b, const ink *c) {
    if (c->mode != SH_COLOR_NONE) sh_buf_puts(b, "\x1b[0m");
}

static void accent(const ink *c, sh_buf *b) { paint(b, c, c->r, c->g, c->b); }

static void read_accent(ink *c) {
    char *config = sh_config_dir();
    char *path = sh_path_join(config, "settings.json");
    yyjson_doc *doc = NULL;
    const char *seed = NULL;
    if (sh_json_read(path, 0, &doc) == SH_JSON_OK) {
        yyjson_val *look = yyjson_obj_get(yyjson_doc_get_root(doc), "appearance");
        yyjson_val *a = look ? yyjson_obj_get(look, "accent") : NULL;
        if (a && strcmp(sh_json_str(a, "source"), "custom") == 0) seed = sh_json_str(a, "color");
    }
    sh_palette p;
    unsigned r = 0x34, g = 0xE0, bl = 0xA6;
    if (seed && sh_palette_derive(seed, true, &p)) sscanf(p.mark + 1, "%2x%2x%2x", &r, &g, &bl);
    c->r = (int)r;
    c->g = (int)g;
    c->b = (int)bl;
    yyjson_doc_free(doc);
    free(path);
    free(config);
}

// ---- the logo ----

// two stems and the floating crossbar, as half blocks
static const char *const LOGO[LOGO_H] = {"s▄▄      ▄▄", "s██      ██", "s██ a▄▄▄▄s ██", "s██ a▀▀▀▀s ██", "s██      ██", "s▀▀      ▀▀"};

static void put_logo_line(sh_buf *b, const ink *c, int row) {
    sh_buf_puts(b, "  ");
    for (const char *p = LOGO[row]; *p; ++p) {
        if (*p == 's') paint(b, c, 232, 236, 241);
        else if (*p == 'a') accent(c, b);
        else sh_buf_putc(b, *p);
    }
    plain(b, c);
}

// an image as half blocks: two pixels to a cell, each with its own colour
static bool image_lines(const char *path, const ink *c, sh_strv *out) {
    sh_buf file = SH_BUF_INIT;
    if (!sh_read_file(path, 32 * 1024 * 1024, &file)) return false;
    int w = 0, h = 0, comp = 0;
    unsigned char *px = stbi_load_from_memory((const unsigned char *)file.data, (int)file.len, &w, &h, &comp, 4);
    sh_buf_free(&file);
    if (!px) return false;
    for (int y = 0; y < LOGO_H; ++y) {
        sh_buf line = SH_BUF_INIT;
        sh_buf_puts(&line, "  ");
        for (int x = 0; x < LOGO_W; ++x) {
            const unsigned char *top = px + 4 * ((size_t)(y * 2 * h / (LOGO_H * 2)) * (size_t)w + (size_t)(x * w / LOGO_W));
            const unsigned char *low = px + 4 * ((size_t)((y * 2 + 1) * h / (LOGO_H * 2)) * (size_t)w + (size_t)(x * w / LOGO_W));
            if (c->mode == SH_COLOR_TRUE) sh_buf_printf(&line, "\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm\xe2\x96\x80", top[0], top[1], top[2], low[0], low[1], low[2]);
            else sh_buf_puts(&line, (top[3] > 127 || low[3] > 127) ? "\xe2\x96\x88" : " ");
        }
        plain(&line, c);
        sh_strv_push(out, sh_buf_str(&line));
        sh_buf_free(&line);
    }
    stbi_image_free(px);
    return true;
}

// The logo as an image, through the kitty graphics protocol: crisp, at the size of the half-block one. The file's own bytes for
// a PNG; the built-in mark is drawn here, four pixels of colour at a time.
static void put_chunks(sh_buf *out, const char *first, const unsigned char *data, size_t n) {
    sh_buf b64 = SH_BUF_INIT;
    sh_base64_encode(data, n, &b64);
    for (size_t at = 0; at < b64.len || at == 0; at += 4096) {
        const size_t part = b64.len - at < 4096 ? b64.len - at : 4096;
        sh_buf_printf(out, "\x1b_G%s%sm=%d;", at == 0 ? first : "", at == 0 ? "," : "", at + 4096 < b64.len);
        sh_buf_append(out, b64.data + at, part);
        sh_buf_puts(out, "\x1b\\");
    }
    sh_buf_free(&b64);
}

static bool kitty_logo(sh_buf *out, const char *logo, const ink *c) {
    sh_buf_printf(out, "  ");
    if (strcmp(logo, "builtin") != 0) {
        if (!sh_ends_with(logo, ".png")) return false;
        sh_buf file = SH_BUF_INIT;
        if (!sh_read_file(logo, 32 * 1024 * 1024, &file)) return false;
        char *first = sh_strfmt("a=T,f=100,c=%d,r=%d,q=2", LOGO_W, LOGO_H);
        put_chunks(out, first, (const unsigned char *)file.data, file.len);
        free(first);
        sh_buf_free(&file);
        return true;
    }
    enum { W = 96, H = 96 };
    unsigned char *px = sh_xcalloc(W * H, 4);
    for (int y = 8; y < H - 8; ++y) {
        for (int x = 0; x < W; ++x) {
            const bool stem = x < 18 || x >= W - 18;
            const bool bar = x >= 34 && x < W - 34 && y >= 38 && y < H - 38;
            if (!stem && !bar) continue;
            unsigned char *p = px + 4 * (y * W + x);
            p[0] = (unsigned char)(bar ? c->r : 232);
            p[1] = (unsigned char)(bar ? c->g : 236);
            p[2] = (unsigned char)(bar ? c->b : 241);
            p[3] = 255;
        }
    }
    char *first = sh_strfmt("a=T,f=32,s=%d,v=%d,c=%d,r=%d,q=2", W, H, LOGO_W, LOGO_H);
    put_chunks(out, first, px, W * H * 4);
    free(first);
    free(px);
    return true;
}

// ---- the banner ----

typedef struct {
    const char *key;
    char *value;
    const char *nerd;
} row;

static void put_dots(sh_buf *b, const ink *c) {
    static const int dots[7][3] = {{255, 106, 94}, {255, 178, 36}, {200, 242, 60}, {52, 224, 166}, {0, 0, 0}, {165, 176, 190}, {232, 236, 241}};
    for (int i = 0; i < 7; ++i) {
        if (i == 4) accent(c, b);
        else paint(b, c, dots[i][0], dots[i][1], dots[i][2]);
        sh_buf_puts(b, i ? " \xe2\x97\x8f" : "\xe2\x97\x8f");
    }
    plain(b, c);
}

char *sh_fetch_render(const sh_store *st, const sh_fetch_opts *o) {
    facts f;
    gather(st, &f);
    ink c = {o->color, 0, 0, 0};
    read_accent(&c);
    char *shown_folder = shown_bytes(f.folder_bytes), *shown_disk = shown_bytes(f.disk_total), *shown_backup = shown_bytes(f.backup_bytes), *shown_cache = shown_bytes(f.cache_bytes);
    const double share = f.disk_total > 0 ? 100.0 * (double)f.folder_bytes / (double)f.disk_total : 0;
    row rows[] = {
        {"OS", sh_strdup(f.sys.os), "\xef\x8c\x9a "},
        {"Kernel", sh_strdup(f.sys.kernel), "\xef\x80\x93 "},
        {"Shell", sh_strdup(f.sys.shell), "\xef\x84\xa0 "},
        {"Terminal", sh_strdup(f.sys.terminal), "\xef\x92\x89 "},
        {"Skills", sh_strfmt("%d (%d tracked, %d local) · %d agents of %d known", f.skills, f.tracked, f.local, f.agents, f.known), "\xef\x83\xab "},
        {"Updates", updates_text(&f), "\xef\x80\x99 "},
        {"Folder", sh_strfmt("%s · %s · %.1f %% of %s", f.folder, shown_folder, share, shown_disk), "\xef\x81\xbb "},
        {"Backups", sh_strfmt("%s · Cache %s · %s", shown_backup, shown_cache, f.problems ? "links need repair" : "Links healthy"), "\xef\x87\x80 "},
    };
    const size_t n_rows = sizeof rows / sizeof rows[0];

    sh_strv logo = SH_STRV_INIT;
    const bool no_logo = strcmp(o->logo, "none") == 0;
    bool image_drawn = false;
    sh_buf out = SH_BUF_INIT;
    if (!no_logo && o->kitty_image) {
        sh_buf img = SH_BUF_INIT;
        if (kitty_logo(&img, o->logo, &c)) {
            sh_buf_puts(&out, "\x1b" "7");  // the image moves the cursor: everything else goes back to where it started
            sh_buf_append(&out, img.data, img.len);
            sh_buf_puts(&out, "\x1b" "8");
            image_drawn = true;
        }
        sh_buf_free(&img);
    }
    if (!no_logo && !image_drawn && !(strcmp(o->logo, "builtin") != 0 && image_lines(o->logo, &c, &logo))) {
        for (int i = 0; i < LOGO_H; ++i) {
            sh_buf line = SH_BUF_INIT;
            put_logo_line(&line, &c, i);
            sh_strv_push(&logo, sh_buf_str(&line));
            sh_buf_free(&line);
        }
    }
    const int width = image_drawn || logo.len ? LOGO_W + 2 + GAP : 0;
    const size_t lines = n_rows + 1 + (o->icons == SH_ICONS_NONE ? 0 : 2);
    for (size_t i = 0; i < lines; ++i) {
        if (image_drawn) sh_buf_printf(&out, "\x1b[%dC", width);
        else if (i < logo.len) { sh_buf_puts(&out, logo.items[i]); sh_buf_printf(&out, "%*s", GAP, ""); }
        else if (width) sh_buf_printf(&out, "%*s", width, "");
        size_t k = i;
        if (o->icons != SH_ICONS_NONE && k == 0) {
            put_dots(&out, &c);
        } else {
            if (o->icons != SH_ICONS_NONE) --k;
            if (k == 0) {
                accent(&c, &out);
                sh_buf_printf(&out, "%s@%s", f.sys.user, f.sys.host);
                plain(&out, &c);
            } else if (k - 1 < n_rows) {
                const row *r = &rows[k - 1];
                accent(&c, &out);
                if (o->icons == SH_ICONS_NERD) sh_buf_puts(&out, r->nerd);
                sh_buf_printf(&out, "%-10s", r->key);
                plain(&out, &c);
                sh_buf_puts(&out, r->value);
            } else if (o->icons != SH_ICONS_NONE) {
                put_dots(&out, &c);
            }
        }
        sh_buf_putc(&out, '\n');
    }
    for (size_t i = 0; i < n_rows; ++i) free(rows[i].value);
    free(shown_folder);
    free(shown_disk);
    free(shown_backup);
    free(shown_cache);
    sh_strv_free(&logo);
    facts_free(&f);
    char *text = sh_strdup(sh_buf_str(&out));
    sh_buf_free(&out);
    return text;
}

static yyjson_mut_val *facts_json(yyjson_mut_doc *d, const facts *f) {
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, o, "user", f->sys.user);
    yyjson_mut_obj_add_strcpy(d, o, "host", f->sys.host);
    yyjson_mut_obj_add_strcpy(d, o, "os", f->sys.os);
    yyjson_mut_obj_add_strcpy(d, o, "kernel", f->sys.kernel);
    yyjson_mut_obj_add_strcpy(d, o, "shell", f->sys.shell);
    yyjson_mut_obj_add_strcpy(d, o, "terminal", f->sys.terminal);
    yyjson_mut_obj_add_strcpy(d, o, "uptime", f->sys.uptime);
    yyjson_mut_obj_add_int(d, o, "skills", f->skills);
    yyjson_mut_obj_add_int(d, o, "tracked", f->tracked);
    yyjson_mut_obj_add_int(d, o, "local", f->local);
    yyjson_mut_obj_add_int(d, o, "agents", f->agents);
    yyjson_mut_obj_add_int(d, o, "agentsKnown", f->known);
    yyjson_mut_obj_add_int(d, o, "linkProblems", f->problems);
    yyjson_mut_obj_add_int(d, o, "updates", f->updates);
    yyjson_mut_obj_add_strcpy(d, o, "checked", f->checked);
    yyjson_mut_obj_add_strcpy(d, o, "folder", f->folder);
    yyjson_mut_obj_add_int(d, o, "folderBytes", f->folder_bytes);
    yyjson_mut_obj_add_int(d, o, "backupBytes", f->backup_bytes);
    yyjson_mut_obj_add_int(d, o, "cacheBytes", f->cache_bytes);
    yyjson_mut_obj_add_int(d, o, "diskTotal", f->disk_total);
    yyjson_mut_obj_add_int(d, o, "diskFree", f->disk_free);
    return o;
}

char *sh_fetch_json(const sh_store *st) {
    facts f;
    gather(st, &f);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(d, facts_json(d, &f));
    char *json = yyjson_mut_write(d, YYJSON_WRITE_PRETTY_TWO_SPACES, NULL);
    char *out = sh_strdup(json ? json : "{}");
    free(json);
    yyjson_mut_doc_free(d);
    facts_free(&f);
    return out;
}

char *sh_fetch_field(const sh_store *st, const char *name) {
    facts f;
    gather(st, &f);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = facts_json(d, &f);
    yyjson_mut_val *v = yyjson_mut_obj_get(o, name);
    char *out = NULL;
    if (yyjson_mut_is_str(v)) out = sh_strdup(yyjson_mut_get_str(v));
    else if (yyjson_mut_is_int(v)) out = sh_strfmt("%lld", (long long)yyjson_mut_get_sint(v));
    yyjson_mut_doc_free(d);
    facts_free(&f);
    return out;
}
