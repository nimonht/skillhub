// What the window shows, built from a scan: one <Skill> row per skill, the agent columns, the providers and the counts
// (docs/protocol.md). Ported from 1.x AppController::rebuildDerived and SkillModel; the window only presents it.
#include <stdlib.h>
#include <string.h>

#include "serve.h"
#include "sh_fs.h"
#include "sh_history.h"
#include "sh_json.h"
#include "sh_path.h"
#include "sh_time.h"
#include "sh_utf.h"

#define MAX_COLUMNS 6  // agent columns in the list; more fold into one overflow column

// ---- remembered check results ----

static int cmp_entry(const void *a, const void *b) { return strcmp(((const check_entry *)a)->name, ((const check_entry *)b)->name); }

static check_entry *check_find(const check_map *m, const char *name) {
    const check_entry key = {(char *)name, NULL, NULL, NULL, NULL};
    return m->len ? bsearch(&key, m->items, m->len, sizeof *m->items, cmp_entry) : NULL;
}

static void entry_free(check_entry *e) {
    free(e->name);
    free(e->status);
    free(e->note);
    free(e->lock_hash);
    free(e->new_hash);
}

void check_map_add(check_map *m, const char *name, const char *status, const char *note, const char *lock_hash, const char *new_hash) {
    if (m->len == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 64;
        m->items = sh_xrealloc(m->items, m->cap * sizeof *m->items);
    }
    m->items[m->len++] = (check_entry){sh_strdup(name), sh_strdup(status), sh_strdup(note ? note : ""),
                                       sh_strdup(lock_hash ? lock_hash : ""), sh_strdup(new_hash ? new_hash : "")};
}

typedef struct {
    check_entry e;
    size_t pos;
} positioned;

static int cmp_positioned(const void *a, const void *b) {
    const positioned *x = a, *y = b;
    const int c = strcmp(x->e.name, y->e.name);
    return c ? c : (x->pos < y->pos ? -1 : x->pos > y->pos);
}

void check_map_sort(check_map *m) {
    positioned *p = sh_xcalloc(m->len + 1, sizeof *p);
    for (size_t i = 0; i < m->len; ++i) p[i] = (positioned){m->items[i], i};
    qsort(p, m->len, sizeof *p, cmp_positioned);
    size_t kept = 0;
    for (size_t i = 0; i < m->len; ++i) {
        if (i + 1 < m->len && strcmp(p[i].e.name, p[i + 1].e.name) == 0) entry_free(&p[i].e);  // a later result wins
        else m->items[kept++] = p[i].e;
    }
    m->len = kept;
    free(p);
}

void check_map_merge(check_map *dst, check_map *src) {
    check_entry *out = sh_xcalloc(dst->len + src->len + 1, sizeof *out);
    size_t i = 0, j = 0, n = 0;
    while (i < dst->len || j < src->len) {
        const int c = i == dst->len ? 1 : j == src->len ? -1 : strcmp(dst->items[i].name, src->items[j].name);
        if (c < 0) {
            out[n++] = dst->items[i++];
        } else {
            if (c == 0) entry_free(&dst->items[i++]);
            out[n++] = src->items[j++];
        }
    }
    free(dst->items);
    free(src->items);
    const size_t cap = dst->len + src->len + 1;
    dst->items = out;
    dst->len = n;
    dst->cap = cap;
    memset(src, 0, sizeof *src);
}

void check_map_remove(check_map *m, const char *name) {
    check_entry *e = check_find(m, name);
    if (!e) return;
    entry_free(e);
    const size_t at = (size_t)(e - m->items);
    memmove(e, e + 1, (m->len - at - 1) * sizeof *e);
    --m->len;
}

void check_map_free(check_map *m) {
    for (size_t i = 0; i < m->len; ++i) entry_free(&m->items[i]);
    free(m->items);
    memset(m, 0, sizeof *m);
}

check_map check_map_copy(const check_map *m) {
    check_map c = {sh_xcalloc(m->len + 1, sizeof *m->items), m->len, m->len + 1};
    for (size_t i = 0; i < m->len; ++i) {
        const check_entry *e = &m->items[i];
        c.items[i] = (check_entry){sh_strdup(e->name), sh_strdup(e->status), sh_strdup(e->note), sh_strdup(e->lock_hash), sh_strdup(e->new_hash)};
    }
    return c;
}

// ---- presentation details the window shows for an agent ----

static size_t char_len(const char *s) {  // bytes of the UTF-8 character at s
    const char *p = s;
    sh_utf8_next(&p, s + strlen(s));
    return (size_t)(p - s);
}

// two letters: initials of the first two words, or of two camel-case parts ("OpenCode" -> OC), else the first two letters
static char *monogram(const char *label) {
    const char *starts[2] = {NULL, NULL};
    size_t parts = 0;
    bool in_part = false;
    char prev = 0;
    for (const char *c = label; *c && parts < 2; ++c) {
        const bool sep = *c == ' ' || *c == '\t' || *c == '-';
        const bool camel = in_part && *c >= 'A' && *c <= 'Z' && prev >= 'a' && prev <= 'z';
        if (sep) {
            in_part = false;
        } else if (!in_part || camel) {
            starts[parts++] = c;
            in_part = true;
        }
        prev = *c;
    }
    sh_buf b = SH_BUF_INIT;
    if (parts == 2) {
        for (size_t i = 0; i < 2; ++i) sh_buf_append(&b, starts[i], char_len(starts[i]));
    } else {
        const char *c = label;
        for (int i = 0; i < 2 && *c; ++i) {
            const size_t n = char_len(c);
            sh_buf_append(&b, c, n);
            c += n;
        }
    }
    sh_buf_append(&b, "", 0);
    for (char *c = b.data; *c; ++c)
        if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
    return sh_buf_steal(&b);
}

// the name under a column: the vendor's own short form when the full label would be cut off ("Claude Code" -> "Claude")
static char *short_label(const char *label) {
    static const char *const tails[] = {" Code CLI", " for Terminal", " Code", " CLI", " Agent", " Build", " Vibe"};
    const char *s = sh_starts_with(label, "GitHub ") ? label + 7 : label;
    if (sh_utf16_len(s) <= 9) return sh_strdup(s);
    const size_t n = strlen(s);
    for (size_t i = 0; i < sizeof tails / sizeof tails[0]; ++i) {
        const size_t t = strlen(tails[i]);
        if (n > t && sh_ends_with(s, tails[i])) return sh_strndup(s, n - t);
    }
    return sh_strdup(s);
}

static char *mechanism_of(const sh_agent *a) {
    char *where = sh_tilde(strcmp(a->mode, "config") == 0 ? a->config_file : a->dir);
    char *m = strcmp(a->mode, "native") == 0   ? sh_strdup("Reads the shared folder directly")
              : strcmp(a->mode, "config") == 0 ? sh_strfmt("Shared folder listed in %s", where)
                                               : sh_strfmt("Symlinks in %s", where);
    free(where);
    return m;
}

static const char *link_key(sh_link_state s) {
    switch (s) {
    case SH_LINK_NATIVE: return "shared";
    case SH_LINK_LINKED: return "link";
    case SH_LINK_MISSING: return "missing";
    case SH_LINK_COPY: return "copy";
    case SH_LINK_FOREIGN: return "foreign";
    case SH_LINK_OFF: return "off";
    }
    return "";
}

static bool bad_link(sh_link_state s) { return s == SH_LINK_MISSING || s == SH_LINK_COPY || s == SH_LINK_FOREIGN; }

// ---- the status a row shows ----

// The check result applies while the lock entry still has the hash it was checked against. When the entry moved on to
// the hash upstream had (another process updated it), the skill is current; when it moved anywhere else, the result is
// stale and the skill shows as tracked again.
static const char *status_of(const sh_skill *s, const check_map *checks, const sh_strv *just_updated, const char **note, bool *stale) {
    *note = "";
    *stale = false;
    if (!sh_source_tracked(&s->src)) return "local";
    const check_entry *c = check_find(checks, s->name);
    if (!c) return "tracked";
    const bool updated = sh_strv_contains(just_updated, s->name);
    if (strcmp(s->src.hash, c->lock_hash) == 0) {
        if (updated && strcmp(c->status, "current") == 0) return "updated";
        *note = c->note;
        return c->status;
    }
    if (*c->new_hash && strcmp(s->src.hash, c->new_hash) == 0) return updated ? "updated" : "current";
    *stale = true;
    return "tracked";
}

// ---- building a scan ----

static char *json_of(yyjson_mut_doc *d, yyjson_mut_val *v) {
    char *s = yyjson_mut_val_write(v, YYJSON_WRITE_ALLOW_INVALID_UNICODE, NULL);
    (void)d;
    return s ? s : sh_strdup("null");
}

static int cmp_cstr(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

// how many backups each skill has: the backup root lists one folder per skill name
static size_t versions_of(const sh_strv *backed_up, const char *backup_root, const char *name) {
    const char *key = name;
    if (!backed_up->len || !bsearch(&key, backed_up->items, backed_up->len, sizeof *backed_up->items, cmp_cstr)) return 0;
    char *dir = sh_path_join(backup_root, name);
    sh_strv items = SH_STRV_INIT;
    sh_list_dir(dir, SH_LIST_DIRS, &items);
    const size_t n = items.len;
    sh_strv_free(&items);
    free(dir);
    return n;
}

static void add_watch(scan_result *r, const char *dir, const char *file) {
    if (!dir || !*dir) return;
    const ptrdiff_t at = sh_strv_index(&r->watch_dirs, dir);
    if (at < 0) {
        sh_strv_push(&r->watch_dirs, dir);
        sh_strv_push(&r->watch_files, file);
    } else if (strcmp(r->watch_files.items[at], file) != 0) {  // two interests in one folder: any change counts
        free(r->watch_files.items[at]);
        r->watch_files.items[at] = sh_strdup("");
    }
}

static void add_watch_file(scan_result *r, const char *path) {
    if (!path || !*path) return;
    char *dir = sh_path_parent(path);
    add_watch(r, dir, sh_path_name(path));
    free(dir);
}

typedef struct {
    int updates, local, problems, checked;
} tally;

static yyjson_mut_val *row_json(yyjson_mut_doc *d, const sh_store *st, const sh_skill *s, const char *status, const char *note,
                                size_t versions, bool hold, bool *problem) {
    yyjson_mut_val *o = yyjson_mut_obj(d);
    const bool tracked = sh_source_tracked(&s->src);
    yyjson_mut_obj_add_strcpy(d, o, "name", s->name);
    yyjson_mut_obj_add_strcpy(d, o, "description", s->description);
    yyjson_mut_obj_add_strcpy(d, o, "source", tracked ? s->src.source : "");
    yyjson_mut_obj_add_strcpy(d, o, "sourceType", !tracked ? "" : *s->src.type ? s->src.type : "github");
    yyjson_mut_obj_add_bool(d, o, "live", s->live);
    yyjson_mut_obj_add_str(d, o, "status", status);
    yyjson_mut_obj_add_strcpy(d, o, "note", note);
    yyjson_mut_val *links = yyjson_mut_arr(d);
    *problem = strcmp(status, "error") == 0;
    for (size_t k = 0; k < s->n_links; ++k) {
        yyjson_mut_arr_add_str(d, links, link_key(s->links[k]));
        if (bad_link(s->links[k])) *problem = true;
    }
    yyjson_mut_obj_add_val(d, o, "links", links);
    yyjson_mut_obj_add_bool(d, o, "problem", *problem);
    yyjson_mut_obj_add_bool(d, o, "parked", s->parked);
    yyjson_mut_obj_add_bool(d, o, "hold", hold);
    yyjson_mut_obj_add_uint(d, o, "versions", versions);
    yyjson_mut_obj_add_str(d, o, "lock", "current");
    char *path = sh_tilde(s->path);
    yyjson_mut_obj_add_strcpy(d, o, "path", path);
    free(path);
    yyjson_mut_obj_add_strcpy(d, o, "upstreamPath", s->src.skill_path ? s->src.skill_path : "");
    yyjson_mut_obj_add_strcpy(d, o, "hash", s->src.hash ? s->src.hash : "");
    yyjson_mut_obj_add_strcpy(d, o, "installedAt", s->src.installed_at ? s->src.installed_at : "");
    yyjson_mut_obj_add_strcpy(d, o, "updatedAt", s->src.updated_at ? s->src.updated_at : "");
    (void)st;
    return o;
}

static void build_rows(scan_result *r, const sh_store *st, const sh_skills *sk, const check_map *checks, const sh_strv *just_updated,
                       tally *t) {
    sh_strv backed_up = SH_STRV_INIT;
    sh_list_dir(st->backup, SH_LIST_DIRS | SH_SORT_NAME, &backed_up);
    sh_holds *holds = sh_holds_load(st);
    r->rows = sh_xcalloc(sk->len + 1, sizeof *r->rows);
    for (size_t i = 0; i < sk->len; ++i) {
        const sh_skill *s = &sk->items[i];
        const char *note;
        bool stale, problem;
        const char *status = status_of(s, checks, just_updated, &note, &stale);
        if (stale) sh_strv_push(&r->stale, s->name);
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *o = row_json(d, st, s, status, note, versions_of(&backed_up, st->backup, s->name), sh_holds_get(holds, s->name) != NULL, &problem);
        r->rows[r->n].name = sh_strdup(s->name);
        r->rows[r->n].json = json_of(d, o);
        ++r->n;
        yyjson_mut_doc_free(d);
        t->updates += strcmp(status, "available") == 0;
        t->local += strcmp(status, "local") == 0;
        t->problems += problem;
        t->checked += strcmp(status, "current") == 0 || strcmp(status, "available") == 0 || strcmp(status, "error") == 0 ||
                      strcmp(status, "updated") == 0;
    }
    sh_holds_free(holds);
    sh_strv_free(&backed_up);
}

static void build_agents(scan_result *r, const sh_store *st, const sh_skills *sk) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *tiles = yyjson_mut_arr(d);
    int hidden_bad = 0, hidden_issue = 0;
    sh_strv hidden_names = SH_STRV_INIT;
    const bool folding = st->n_agents > MAX_COLUMNS;
    for (size_t k = 0; k < st->n_agents; ++k) {
        const sh_agent *a = st->agents[k];
        int ok = 0, bad = 0;  // a skill turned off for this agent is neither: nothing is wrong, and the agent does not see it
        for (size_t i = 0; i < sk->len; ++i) {
            if (k < sk->items[i].n_links && bad_link(sk->items[i].links[k])) ++bad;
            else if (k >= sk->items[i].n_links || sk->items[i].links[k] != SH_LINK_OFF) ++ok;
        }
        const bool wired = sh_store_provider_wired(st, a);
        const char *health = bad > 0 || !wired ? "issue" : "ok";
        char *short_name = short_label(a->label), *mono = monogram(a->label), *mech = mechanism_of(a);
        char *folder = sh_tilde(strcmp(a->mode, "config") == 0 ? a->config_file : a->dir);
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, o, "id", a->id);
        yyjson_mut_obj_add_strcpy(d, o, "label", a->label);
        yyjson_mut_obj_add_strcpy(d, o, "short", short_name);
        yyjson_mut_obj_add_strcpy(d, o, "monogram", mono);
        yyjson_mut_obj_add_strcpy(d, o, "mode", a->mode);
        yyjson_mut_obj_add_strcpy(d, o, "mechanism", mech);
        yyjson_mut_obj_add_strcpy(d, o, "folder", folder);
        yyjson_mut_obj_add_bool(d, o, "detected", a->detected);
        yyjson_mut_obj_add_strcpy(d, o, "detectedBy", a->detected_by ? a->detected_by : "");
        yyjson_mut_obj_add_bool(d, o, "wired", wired);
        yyjson_mut_obj_add_int(d, o, "ok", ok);
        yyjson_mut_obj_add_int(d, o, "problems", bad);
        yyjson_mut_obj_add_uint(d, o, "total", sk->len);
        yyjson_mut_obj_add_str(d, o, "health", health);
        yyjson_mut_arr_append(tiles, o);
        free(short_name);
        free(mono);
        free(mech);
        free(folder);
        if (folding && k >= MAX_COLUMNS - 1) {
            hidden_bad += bad;
            hidden_issue += strcmp(health, "issue") == 0;
            sh_strv_push(&hidden_names, a->label);
        }
    }
    r->agents = json_of(d, tiles);
    if (folding) {
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, o, "count", st->n_agents - (MAX_COLUMNS - 1));
        yyjson_mut_obj_add_int(d, o, "problems", hidden_bad);
        yyjson_mut_obj_add_str(d, o, "health", hidden_issue > 0 ? "issue" : "ok");
        char *names = sh_strv_join(&hidden_names, ", ");
        yyjson_mut_obj_add_strcpy(d, o, "names", names);
        free(names);
        r->overflow = json_of(d, o);
    } else {
        r->overflow = sh_strdup("null");
    }
    sh_strv_free(&hidden_names);
    yyjson_mut_doc_free(d);
}

static void build_providers(scan_result *r, const sh_store *st) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *list = yyjson_mut_arr(d);
    for (size_t i = 0; i < st->n_providers; ++i) {
        const sh_agent *a = &st->providers[i];
        char *mono = monogram(a->label), *mech = mechanism_of(a);
        char *folder = sh_tilde(strcmp(a->mode, "config") == 0 ? a->config_file : a->dir);
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, o, "id", a->id);
        yyjson_mut_obj_add_strcpy(d, o, "label", a->label);
        yyjson_mut_obj_add_strcpy(d, o, "monogram", mono);
        yyjson_mut_obj_add_strcpy(d, o, "mode", a->mode);
        yyjson_mut_obj_add_strcpy(d, o, "mechanism", mech);
        yyjson_mut_obj_add_strcpy(d, o, "folder", folder);
        yyjson_mut_obj_add_bool(d, o, "detected", a->detected);
        yyjson_mut_obj_add_strcpy(d, o, "detectedBy", a->detected_by ? a->detected_by : "");
        yyjson_mut_obj_add_bool(d, o, "managed", a->managed);
        yyjson_mut_obj_add_bool(d, o, "custom", a->custom);
        yyjson_mut_arr_append(list, o);
        free(mono);
        free(mech);
        free(folder);
    }
    r->providers = json_of(d, list);
    r->managed = (int)st->n_agents;
    r->known = (int)st->n_providers;
    yyjson_mut_doc_free(d);
}

static void build_watches(scan_result *r, const sh_store *st) {
    add_watch(r, st->central, "");
    for (size_t k = 0; k < st->n_agents; ++k) {
        const sh_agent *a = st->agents[k];
        if (a->linked) add_watch(r, a->dir, "");
        for (size_t i = 0; i < a->extra_dirs.len; ++i) add_watch(r, a->extra_dirs.items[i], "");
        if (a->config_file && *a->config_file) add_watch_file(r, a->config_file);
    }
    add_watch_file(r, st->lock_path);  // the folder, never the file: lock files are replaced, not written in place
    add_watch_file(r, st->lock_other);
    char *config = sh_config_dir();
    add_watch(r, config, "");  // providers.json and settings.json
    free(config);
}

scan_result *scan_build(const check_map *checks, const sh_strv *just_updated) {
    sh_store st;
    sh_store_init(&st, NULL, NULL);
    sh_skills sk;
    sh_store_scan(&st, &sk);
    sh_strays strays;
    sh_store_strays(&st, &strays);
    scan_result *r = sh_xcalloc(1, sizeof *r);
    tally t = {0, 0, 0, 0};
    build_rows(r, &st, &sk, checks, just_updated, &t);
    build_agents(r, &st, &sk);
    build_providers(r, &st);
    build_watches(r, &st);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *c = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, c, "skills", sk.len);
    yyjson_mut_obj_add_int(d, c, "updates", t.updates);
    yyjson_mut_obj_add_int(d, c, "local", t.local);
    yyjson_mut_obj_add_int(d, c, "problems", t.problems);
    yyjson_mut_obj_add_uint(d, c, "strays", strays.len);
    yyjson_mut_obj_add_int(d, c, "checked", t.checked);
    yyjson_mut_obj_add_bool(d, c, "wiringOk", sh_store_wiring_ok(&st));
    r->counts = json_of(d, c);
    yyjson_mut_doc_free(d);
    sh_strays_free(&strays);
    sh_skills_free(&sk);
    sh_store_free(&st);
    return r;
}

void scan_result_free(scan_result *r) {
    if (!r) return;
    for (size_t i = 0; i < r->n; ++i) {
        free(r->rows[i].name);
        free(r->rows[i].json);
    }
    free(r->rows);
    free(r->agents);
    free(r->overflow);
    free(r->providers);
    free(r->counts);
    sh_strv_free(&r->watch_dirs);
    sh_strv_free(&r->watch_files);
    sh_strv_free(&r->stale);
    free(r);
}

// ---- deltas and pages ----

static int cmp_row(const void *a, const void *b) { return strcmp((*(const row *const *)a)->name, (*(const row *const *)b)->name); }

static const row **sorted_rows(const scan_result *r) {
    const row **p = sh_xcalloc(r->n + 1, sizeof *p);
    for (size_t i = 0; i < r->n; ++i) p[i] = &r->rows[i];
    qsort(p, r->n, sizeof *p, cmp_row);
    return p;
}

static yyjson_mut_val *raw(yyjson_mut_doc *d, const char *json) { return yyjson_mut_rawcpy(d, json); }

yyjson_mut_val *scan_delta(yyjson_mut_doc *d, const scan_result *before, const scan_result *after, long base_rev, long rev) {
    const row **a = sorted_rows(before), **b = sorted_rows(after);
    yyjson_mut_val *upserts = yyjson_mut_arr(d), *removes = yyjson_mut_arr(d);
    size_t i = 0, j = 0, changes = 0;
    while (i < before->n || j < after->n) {
        const int c = i == before->n ? 1 : j == after->n ? -1 : strcmp(a[i]->name, b[j]->name);
        if (c < 0) {
            yyjson_mut_arr_add_strcpy(d, removes, a[i++]->name);
            ++changes;
        } else if (c > 0) {
            yyjson_mut_arr_append(upserts, raw(d, b[j++]->json));
            ++changes;
        } else {
            if (strcmp(a[i]->json, b[j]->json) != 0) {
                yyjson_mut_arr_append(upserts, raw(d, b[j]->json));
                ++changes;
            }
            ++i;
            ++j;
        }
    }
    free(a);
    free(b);
    yyjson_mut_val *p = yyjson_mut_obj(d);
    yyjson_mut_obj_add_int(d, p, "baseRev", base_rev);
    yyjson_mut_obj_add_int(d, p, "rev", rev);
    yyjson_mut_obj_add_val(d, p, "upserts", upserts);
    yyjson_mut_obj_add_val(d, p, "removes", removes);
    // absolute values, sent whenever they changed: the window never adds them up
    if (strcmp(before->counts, after->counts) != 0) {
        yyjson_mut_obj_add_val(d, p, "counts", raw(d, after->counts));
        ++changes;
    }
    if (strcmp(before->agents, after->agents) != 0 || strcmp(before->overflow, after->overflow) != 0) {
        yyjson_mut_obj_add_val(d, p, "agents", raw(d, after->agents));
        yyjson_mut_obj_add_val(d, p, "overflow", raw(d, after->overflow));
        ++changes;
    }
    if (strcmp(before->providers, after->providers) != 0) {
        yyjson_mut_obj_add_val(d, p, "providers", raw(d, after->providers));
        ++changes;
    }
    return changes ? p : NULL;
}

yyjson_mut_val *scan_page(yyjson_mut_doc *d, const scan_result *r, long rev, size_t page, size_t page_size) {
    if (page_size == 0) page_size = 1000;
    const size_t pages = r->n ? (r->n + page_size - 1) / page_size : 1;
    yyjson_mut_val *p = yyjson_mut_obj(d);
    yyjson_mut_obj_add_int(d, p, "rev", rev);
    yyjson_mut_obj_add_str(d, p, "scope", "user");
    yyjson_mut_obj_add_uint(d, p, "page", page);
    yyjson_mut_obj_add_uint(d, p, "pages", pages);
    yyjson_mut_val *skills = yyjson_mut_arr(d);
    for (size_t i = page * page_size; i < r->n && i < (page + 1) * page_size; ++i) yyjson_mut_arr_append(skills, raw(d, r->rows[i].json));
    yyjson_mut_obj_add_val(d, p, "skills", skills);
    if (page == 0) {
        yyjson_mut_obj_add_val(d, p, "agents", raw(d, r->agents));
        yyjson_mut_obj_add_val(d, p, "overflow", raw(d, r->overflow));
        yyjson_mut_obj_add_val(d, p, "providers", raw(d, r->providers));
        yyjson_mut_obj_add_val(d, p, "projects", yyjson_mut_arr(d));
        yyjson_mut_obj_add_val(d, p, "counts", raw(d, r->counts));
    }
    return p;
}

// ---- log lines ----

// as 1.x colored them: failures are errors, conflicts and refusals are warnings
static const char *level_of(const char *text) {
    char *low = sh_strdup(text);
    for (char *c = low; *c; ++c)
        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
    const char *level = sh_starts_with(text, "FAILED") || strstr(low, "failed")         ? "error"
                        : sh_starts_with(text, "CONFLICT") || sh_starts_with(low, "refusing") ? "warn"
                                                                                           : "info";
    free(low);
    return level;
}

char *log_line_json(const char *text) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = yyjson_mut_obj(d);
    char *time = sh_local_hms(), *shown = sh_tilde(text);  // as 1.x: a line that starts with the home folder
    yyjson_mut_obj_add_strcpy(d, o, "time", time);
    yyjson_mut_obj_add_str(d, o, "level", level_of(text));
    yyjson_mut_obj_add_strcpy(d, o, "text", shown);
    char *json = json_of(d, o);
    free(time);
    free(shown);
    yyjson_mut_doc_free(d);
    return json;
}
