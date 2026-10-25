#include "sh_engine.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sh_frontmatter.h"
#include "sh_fs.h"
#include "sh_git.h"
#include "sh_path.h"
#include "sh_thread.h"
#include "sh_time.h"
#include "sh_utf.h"

#define WORKERS 16          // repositories handled at once; each runs one git process at a time
#define FETCHES_PER_HOST 8  // clones and fetches from one host at a time
#define COMMIT_EVERY 200    // lock entries per group commit ...
#define COMMIT_MS 5000      // ... or after this long, whichever comes first
#define DISCOVER_BUDGET 400 // SKILL.md files looked at per repository when listing what it offers, as in 1.x
#define FRONTMATTER_MAX (64 * 1024)

// ---- repositories ----

typedef struct {
    char *path;  // of SKILL.md
    char *name;  // from its front matter ("" when it has none)
    char *description;
} skill_file;

typedef struct {
    char *slug, *url;
    bool tried, ok;
    char *err;          // why it could not be brought up to date, cut like 1.x cut it
    sh_repo repo;
    bool scanned;       // `skills` read
    skill_file *skills; // every SKILL.md in the commit, best location first
    size_t n_skills;
} repo_slot;

typedef struct {
    char *name;
    sh_source src;
} pending_entry;

struct sh_engine {
    sh_store *st;
    repo_slot *slots;
    size_t n_slots, cap_slots;
    sh_limiter *fetches;
    sh_mutex *m;  // guards `pending`
    pending_entry *pending;
    size_t n_pending, cap_pending;
    int64_t last_commit;
};

sh_engine *sh_engine_new(sh_store *st) {
    sh_engine *e = sh_xcalloc(1, sizeof *e);
    e->st = st;
    e->fetches = sh_limiter_new(FETCHES_PER_HOST);
    e->m = sh_mutex_new();
    e->last_commit = sh_mono_ms();
    return e;
}

static void slot_free(repo_slot *s) {
    free(s->slug);
    free(s->url);
    free(s->err);
    sh_repo_free(&s->repo);
    for (size_t i = 0; i < s->n_skills; ++i) {
        free(s->skills[i].path);
        free(s->skills[i].name);
        free(s->skills[i].description);
    }
    free(s->skills);
}

void sh_engine_free(sh_engine *e) {
    if (!e) return;
    for (size_t i = 0; i < e->n_slots; ++i) slot_free(&e->slots[i]);
    free(e->slots);
    for (size_t i = 0; i < e->n_pending; ++i) {
        free(e->pending[i].name);
        sh_source_free(&e->pending[i].src);
    }
    free(e->pending);
    sh_limiter_free(e->fetches);
    sh_mutex_free(e->m);
    free(e);
}

// the slot of a repository, added on first use (calling thread only, before any task runs)
static size_t slot_for(sh_engine *e, const char *slug, const char *url) {
    for (size_t i = 0; i < e->n_slots; ++i)
        if (strcmp(e->slots[i].slug, slug) == 0) return i;
    if (e->n_slots == e->cap_slots) {
        e->cap_slots = e->cap_slots ? e->cap_slots * 2 : 16;
        e->slots = sh_xrealloc(e->slots, e->cap_slots * sizeof *e->slots);
    }
    repo_slot *s = &e->slots[e->n_slots];
    memset(s, 0, sizeof *s);
    s->slug = sh_strdup(slug);
    s->url = sh_strdup(url);
    return e->n_slots++;
}

static char *repo_dir_for(const sh_store *st, const char *slug) {
    sh_buf k = SH_BUF_INIT;
    for (const char *c = slug; *c; ++c) {
        if (*c == '/') sh_buf_puts(&k, "__");
        else sh_buf_putc(&k, *c);
    }
    char *dir = sh_strfmt("%s/repos/%s", st->cache, sh_buf_str(&k));
    sh_buf_free(&k);
    return dir;
}

// brings a repository up to date once per engine; only the task that owns the slot calls this
static bool ensure_synced(sh_engine *e, repo_slot *s) {
    if (s->tried) return s->ok;
    s->tried = true;
    char *dir = repo_dir_for(e->st, s->slug);
    if (!sh_repo_cloned(dir)) sh_store_log(e->st, "cloning %s ...", s->slug);
    char *err = NULL;
    s->ok = sh_repo_open(&s->repo, dir, s->url, e->fetches, &err);
    if (!s->ok) {
        char *shown = sh_left_utf16(err ? err : "", 200);
        sh_store_log(e->st, "git failed for %s: %s", s->slug, shown);
        free(shown);
        s->err = sh_left_utf16(err ? err : "", 300);
    }
    free(err);
    free(dir);
    return s->ok;
}

// ---- where skills sit inside a repository ----

// rank a SKILL.md location; lower is better, NEVER = not a real skill (tests, examples, vendored packages)
#define NEVER (1 << 30)

static int path_rank(const char *p) {
    static const char *const bad[] = {"tests", "test", "evals", "eval", "fixtures", "node_modules", "oracle", "examples", "example"};
    sh_strv seg = SH_STRV_INIT;
    sh_split(p, strlen(p), '/', false, &seg);
    int rank = (int)sh_utf16_len(p);
    for (size_t i = 0; i + 1 < seg.len; ++i) {
        for (size_t b = 0; b < sizeof bad / sizeof bad[0]; ++b) {
            if (strcmp(seg.items[i], bad[b]) == 0) {
                sh_strv_free(&seg);
                return NEVER;
            }
        }
        if (seg.items[i][0] == '.') rank += 1000;
    }
    if (seg.len >= 3 && strcmp(seg.items[seg.len - 3], "skills") == 0) rank -= 200;
    sh_strv_free(&seg);
    return rank;
}

typedef struct {
    int rank;
    const sh_tree_entry *entry;
} ranked_entry;

static int cmp_ranked(const void *a, const void *b) {
    const ranked_entry *x = a, *y = b;
    if (x->rank != y->rank) return x->rank < y->rank ? -1 : 1;
    return strcmp(x->entry->path, y->entry->path);
}

typedef struct {
    skill_file *files;
    sh_buf text;  // the first FRONTMATTER_MAX bytes of the file being read
} fm_reader;

static void fm_begin(void *user, size_t i, long long size) {
    fm_reader *r = user;
    (void)i;
    (void)size;
    sh_buf_clear(&r->text);
}

static void fm_data(void *user, size_t i, const char *p, size_t n) {
    fm_reader *r = user;
    (void)i;
    if (r->text.len < FRONTMATTER_MAX) sh_buf_append(&r->text, p, n < FRONTMATTER_MAX - r->text.len ? n : FRONTMATTER_MAX - r->text.len);
}

static void fm_end(void *user, size_t i) {
    fm_reader *r = user;
    sh_frontmatter fm;
    sh_fm_parse(sh_buf_str(&r->text), r->text.len, &fm);
    r->files[i].name = sh_strdup(sh_fm_value(&fm, "name"));
    r->files[i].description = sh_strdup(sh_fm_value(&fm, "description"));
    sh_fm_free(&fm);
}

// every SKILL.md of the commit, best location first, with the front matter of each read through one git process
static void scan_skills(repo_slot *s) {
    if (s->scanned) return;
    s->scanned = true;
    const sh_repo *r = &s->repo;
    ranked_entry *ranked = sh_xcalloc(r->n + 1, sizeof *ranked);
    size_t n = 0;
    for (size_t i = 0; i < r->n; ++i) {
        const sh_tree_entry *t = &r->entries[i];
        if (t->mode == SH_MODE_TREE || (strcmp(t->path, "SKILL.md") != 0 && !sh_ends_with(t->path, "/SKILL.md"))) continue;
        const int rank = path_rank(t->path);
        if (rank != NEVER) ranked[n++] = (ranked_entry){rank, t};
    }
    qsort(ranked, n, sizeof *ranked, cmp_ranked);
    s->skills = sh_xcalloc(n + 1, sizeof *s->skills);
    s->n_skills = n;
    const char **ids = sh_xcalloc(n + 1, sizeof *ids);
    for (size_t i = 0; i < n; ++i) {
        s->skills[i].path = sh_strdup(ranked[i].entry->path);
        ids[i] = ranked[i].entry->id;
    }
    fm_reader fr = {s->skills, SH_BUF_INIT};
    const sh_blob_reader rd = {fm_begin, fm_data, fm_end, &fr};
    sh_repo_read(r, ids, n, &rd, NULL);  // a file that could not be read has no front matter, as in 1.x
    for (size_t i = 0; i < n; ++i) {     // ... and whatever was not reached at all
        if (!s->skills[i].name) s->skills[i].name = sh_strdup("");
        if (!s->skills[i].description) s->skills[i].description = sh_strdup("");
    }
    sh_buf_free(&fr.text);
    free(ids);
    free(ranked);
}

static bool name_matches(const skill_file *f, const char *name) {
    if (strcmp(f->name, name) == 0) return true;
    if (*f->name) return false;
    char *dir = sh_dir_of(f->path);
    const bool same = strcmp(sh_path_name(dir), name) == 0;
    free(dir);
    return same;
}

// the SKILL.md of the skill called `name`, wherever upstream keeps it now, or NULL
static const char *find_skill(repo_slot *s, const char *name) {
    scan_skills(s);
    for (size_t i = 0; i < s->n_skills; ++i)
        if (name_matches(&s->skills[i], name)) return s->skills[i].path;
    return NULL;
}

// ---- group commits to the lock file ----

static void queue_entry(sh_engine *e, const char *name, const sh_source *src) {
    sh_mutex_lock(e->m);
    if (e->n_pending == e->cap_pending) {
        e->cap_pending = e->cap_pending ? e->cap_pending * 2 : 64;
        e->pending = sh_xrealloc(e->pending, e->cap_pending * sizeof *e->pending);
    }
    pending_entry *p = &e->pending[e->n_pending++];
    p->name = sh_strdup(name);
    sh_source_copy(src, &p->src);
    sh_mutex_unlock(e->m);
}

static void commit_pending(sh_engine *e) {
    sh_mutex_lock(e->m);
    pending_entry *batch = e->pending;
    const size_t n = e->n_pending;
    e->pending = NULL;
    e->n_pending = e->cap_pending = 0;
    sh_mutex_unlock(e->m);
    e->last_commit = sh_mono_ms();
    if (n == 0) return;
    const char **names = sh_xcalloc(n, sizeof *names);
    const sh_source **srcs = sh_xcalloc(n, sizeof *srcs);
    for (size_t i = 0; i < n; ++i) {
        names[i] = batch[i].name;
        srcs[i] = &batch[i].src;
    }
    sh_lock_put_many(e->st, names, srcs, n);
    for (size_t i = 0; i < n; ++i) {
        free(batch[i].name);
        sh_source_free(&batch[i].src);
    }
    free(names);
    free(srcs);
    free(batch);
}

// runs on the calling thread while the workers are busy
static void maybe_commit(sh_engine *e) {
    sh_mutex_lock(e->m);
    const size_t n = e->n_pending;
    sh_mutex_unlock(e->m);
    if (n >= COMMIT_EVERY || (n > 0 && sh_mono_ms() - e->last_commit >= COMMIT_MS)) commit_pending(e);
}

// ---- tasks: one per repository ----

typedef struct {
    size_t slot;
    size_t *items;  // indexes into the job's item array
    size_t n, cap;
} group;

typedef struct {
    group *groups;
    size_t n;
} groups;

static void group_add(groups *g, size_t slot, size_t item) {
    group *gr = NULL;
    for (size_t i = 0; i < g->n && !gr; ++i)
        if (g->groups[i].slot == slot) gr = &g->groups[i];
    if (!gr) {
        g->groups = sh_xrealloc(g->groups, (g->n + 1) * sizeof *g->groups);
        gr = &g->groups[g->n++];
        memset(gr, 0, sizeof *gr);
        gr->slot = slot;
    }
    if (gr->n == gr->cap) {
        gr->cap = gr->cap ? gr->cap * 2 : 8;
        gr->items = sh_xrealloc(gr->items, gr->cap * sizeof *gr->items);
    }
    gr->items[gr->n++] = item;
}

static void groups_free(groups *g) {
    for (size_t i = 0; i < g->n; ++i) free(g->groups[i].items);
    free(g->groups);
}

typedef struct job job;
struct job {
    sh_engine *e;
    groups g;
    void (*task)(job *j, repo_slot *s, const group *gr);
    void *items;
};

static void run_task(size_t i, void *arg) {
    job *j = arg;
    const group *gr = &j->g.groups[i];
    j->task(j, &j->e->slots[gr->slot], gr);
}

static void on_idle(void *arg) { maybe_commit(((job *)arg)->e); }

static void run_job(job *j) {
    const int workers = j->g.n < WORKERS ? (int)j->g.n : WORKERS;
    sh_parallel_for(j->g.n, workers, run_task, j, on_idle, 250);
    commit_pending(j->e);
}

static void set_str(char **field, const char *value) {
    free(*field);
    *field = sh_strdup(value ? value : "");
}

// ---- check ----

static void check_skill(repo_slot *s, sh_skill *sk) {
    char *dir = sh_dir_of(sk->src.skill_path);
    const char *hash = *sk->src.skill_path ? sh_repo_id(&s->repo, dir) : "";
    const bool exists = *hash && sh_repo_find(&s->repo, sk->src.skill_path);
    if (!exists) {  // upstream moved the folder: look it up by skill name
        const char *found = find_skill(s, sk->name);
        if (!found) {
            sk->update = SH_UPDATE_ERROR;
            set_str(&sk->note, "not found upstream");
            free(dir);
            return;
        }
        set_str(&sk->new_path, found);
        free(dir);
        dir = sh_dir_of(found);
        hash = sh_repo_id(&s->repo, dir);
    }
    set_str(&sk->new_hash, hash);
    sk->update = strcmp(hash, sk->src.hash) == 0 && !*sk->new_path ? SH_UPDATE_CURRENT : SH_UPDATE_AVAILABLE;
    free(dir);
}

static void check_task(job *j, repo_slot *s, const group *gr) {
    sh_skill *skills = j->items;
    const bool ok = ensure_synced(j->e, s);
    for (size_t i = 0; i < gr->n; ++i) {
        sh_skill *sk = &skills[gr->items[i]];
        if (ok) {
            check_skill(s, sk);
        } else {
            sk->update = SH_UPDATE_ERROR;
            set_str(&sk->note, s->err);
        }
    }
}

static char *source_url(const sh_source *s) {
    return *s->url ? sh_strdup(s->url) : sh_strfmt("https://github.com/%s.git", s->source);
}

int sh_engine_check(sh_engine *e, sh_skills *skills, const sh_strv *only) {
    job j = {e, {NULL, 0}, check_task, skills->items};
    for (size_t i = 0; i < skills->len; ++i) {
        sh_skill *s = &skills->items[i];
        if (only && only->len > 0 && !sh_strv_contains(only, s->name)) continue;
        set_str(&s->new_hash, "");
        set_str(&s->new_path, "");
        set_str(&s->note, "");
        if (!sh_source_tracked(&s->src)) {
            s->update = SH_UPDATE_UNTRACKED;
            continue;
        }
        char *url = source_url(&s->src);
        group_add(&j.g, slot_for(e, s->src.source, url), i);
        free(url);
    }
    run_job(&j);
    groups_free(&j.g);
    int available = 0;
    for (size_t i = 0; i < skills->len; ++i)
        if (skills->items[i].update == SH_UPDATE_AVAILABLE && (!only || only->len == 0 || sh_strv_contains(only, skills->items[i].name)))
            ++available;
    return available;
}

// ---- installing folders from a repository ----

// One skill folder to bring from a repository into the shared folder.
typedef struct {
    const char *name;
    char *dir;       // folder inside the repository
    char *hash;      // its tree id ("" = not in the commit)
    char *stage;
    bool ok;
} placement;

// Extracts every placement through one git process, then swaps each into the shared folder on its own.
static void place_all(sh_engine *e, repo_slot *s, placement *p, size_t n) {
    sh_extract *x = sh_xcalloc(n + 1, sizeof *x);
    size_t *which = sh_xcalloc(n + 1, sizeof *which);
    size_t k = 0;
    for (size_t i = 0; i < n; ++i) {
        p[i].ok = false;
        if (!*p[i].hash) continue;
        p[i].stage = sh_store_stage_dir(e->st, p[i].name);
        sh_remove_tree(p[i].stage);
        sh_mkpath(p[i].stage);
        x[k] = (sh_extract){p[i].dir, p[i].stage, false, NULL};
        which[k++] = i;
    }
    sh_repo_extract(&s->repo, x, k);
    for (size_t m = 0; m < k; ++m) {
        placement *pl = &p[which[m]];
        if (!x[m].ok) {
            sh_store_log(e->st, "extract failed for %s: %s", pl->name, x[m].err ? x[m].err : "");
            sh_remove_tree(pl->stage);
        } else {
            pl->ok = sh_store_place(e->st, pl->name, pl->stage);
        }
        free(x[m].err);
    }
    free(which);
    free(x);
}

static void placements_free(placement *p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        free(p[i].dir);
        free(p[i].hash);
        free(p[i].stage);
    }
    free(p);
}

// ---- update ----

typedef struct {
    sh_skill *const *todo;
    bool *done;
} update_items;

static void updated(sh_engine *e, sh_skill *sk, const char *skill_md, const char *hash) {
    sh_source src;
    sh_source_copy(&sk->src, &src);
    char *url = source_url(&sk->src);
    set_str(&src.url, url);
    free(url);
    set_str(&src.skill_path, skill_md);
    set_str(&src.hash, hash);
    char *now = sh_now_iso();
    if (!*src.installed_at) set_str(&src.installed_at, now);
    set_str(&src.updated_at, now);
    free(now);
    queue_entry(e, sk->name, &src);
    sh_source_free(&sk->src);
    sk->src = src;
    sk->update = SH_UPDATE_CURRENT;
    set_str(&sk->new_hash, "");
    set_str(&sk->new_path, "");
    char *md = sh_strfmt("%s/%s/SKILL.md", e->st->central, sk->name);
    sh_frontmatter fm;
    sh_fm_read(md, &fm);
    set_str(&sk->description, sh_fm_value(&fm, "description"));
    sh_fm_free(&fm);
    free(md);
    sh_store_log(e->st, "updated %s -> %.8s", sk->name, hash);
}

static void update_task(job *j, repo_slot *s, const group *gr) {
    update_items *it = j->items;
    if (!ensure_synced(j->e, s)) return;
    placement *p = sh_xcalloc(gr->n + 1, sizeof *p);
    char **skill_mds = sh_xcalloc(gr->n + 1, sizeof *skill_mds);
    for (size_t i = 0; i < gr->n; ++i) {
        const sh_skill *sk = it->todo[gr->items[i]];
        skill_mds[i] = sh_strdup(*sk->new_path ? sk->new_path : sk->src.skill_path);
        p[i].name = sk->name;
        p[i].dir = sh_dir_of(skill_mds[i]);
        p[i].hash = sh_strdup(*sk->new_hash ? sk->new_hash : sh_repo_id(&s->repo, p[i].dir));
    }
    place_all(j->e, s, p, gr->n);
    for (size_t i = 0; i < gr->n; ++i) {
        it->done[gr->items[i]] = p[i].ok;
        if (p[i].ok) updated(j->e, it->todo[gr->items[i]], skill_mds[i], p[i].hash);
        free(skill_mds[i]);
    }
    free(skill_mds);
    placements_free(p, gr->n);
}

int sh_engine_update(sh_engine *e, sh_skill *const *todo, size_t n, bool *done) {
    update_items it = {todo, done};
    job j = {e, {NULL, 0}, update_task, &it};
    for (size_t i = 0; i < n; ++i) {
        done[i] = false;
        if (!sh_source_tracked(&todo[i]->src)) continue;
        char *url = source_url(&todo[i]->src);
        group_add(&j.g, slot_for(e, todo[i]->src.source, url), i);
        free(url);
    }
    run_job(&j);
    groups_free(&j.g);
    int ok = 0;
    for (size_t i = 0; i < n; ++i) ok += done[i];
    if (ok) sh_store_relink(e->st);
    return ok;
}

// ---- discover ----

static void discover_task(job *j, repo_slot *s, const group *gr) {
    (void)gr;
    if (ensure_synced(j->e, s)) scan_skills(s);
}

static void candidates_of(const repo_slot *s, sh_discovery *d) {
    sh_strv seen = SH_STRV_INIT;
    d->cands.items = sh_xcalloc(s->n_skills + 1, sizeof *d->cands.items);
    for (size_t i = 0; i < s->n_skills && i < DISCOVER_BUDGET; ++i) {
        const skill_file *f = &s->skills[i];
        char *dir = sh_dir_of(f->path);
        const char *name = *f->name ? f->name : sh_path_name(dir);
        // skill names become folder names: refuse anything path-like
        if (!*name || sh_strv_contains(&seen, name) || strchr(name, '/') || name[0] == '.' || strstr(name, "..")) {
            free(dir);
            continue;
        }
        sh_strv_push(&seen, name);
        sh_candidate *c = &d->cands.items[d->cands.len++];
        c->name = sh_strdup(name);
        c->description = sh_strdup(f->description);
        c->dir = dir;
        c->slug = sh_strdup(s->slug);
        c->url = sh_strdup(s->url);
    }
    if (d->cands.len == 0) d->err = sh_strfmt("no SKILL.md found in %s", s->slug);
    sh_strv_free(&seen);
}

void sh_engine_discover(sh_engine *e, const char *const *inputs, size_t n, sh_discovery *out) {
    job j = {e, {NULL, 0}, discover_task, NULL};
    size_t *slot_of = sh_xcalloc(n + 1, sizeof *slot_of);
    for (size_t i = 0; i < n; ++i) {
        memset(&out[i], 0, sizeof out[i]);
        char *url = NULL;
        char *slug = sh_slug_from_input(inputs[i], &url);
        if (!slug) {
            out[i].err = sh_strdup("expected owner/repo or a github.com URL");
            slot_of[i] = SIZE_MAX;
        } else {
            slot_of[i] = slot_for(e, slug, url);
            group_add(&j.g, slot_of[i], i);
        }
        free(slug);
        free(url);
    }
    run_job(&j);
    groups_free(&j.g);
    for (size_t i = 0; i < n; ++i) {
        if (slot_of[i] == SIZE_MAX) continue;
        const repo_slot *s = &e->slots[slot_of[i]];
        if (!s->ok) out[i].err = sh_strdup(s->err ? s->err : "");
        else candidates_of(s, &out[i]);
    }
    free(slot_of);
}

void sh_discovery_free(sh_discovery *d) {
    sh_candidates_free(&d->cands);
    free(d->err);
    memset(d, 0, sizeof *d);
}

// ---- install and import ----

// One skill to install: from a candidate, or from an import file (whose folder may have moved upstream).
typedef struct {
    const char *name;
    const char *dir;       // folder inside the repository
    bool find_if_moved;    // import: not at `dir` any more -> look it up by name
    const char *source;    // owner/repo, for the log line
    bool done;
    sh_import_why why;
} install_item;

static void installed(sh_engine *e, repo_slot *s, const install_item *it, const char *dir, const char *hash) {
    sh_source src = {0};
    src.source = sh_strdup(s->slug);
    src.url = sh_strdup(s->url);
    src.skill_path = *dir ? sh_strfmt("%s/SKILL.md", dir) : sh_strdup("SKILL.md");
    src.hash = sh_strdup(hash);
    src.installed_at = sh_now_iso();
    src.updated_at = sh_strdup(src.installed_at);
    queue_entry(e, it->name, &src);
    sh_source_free(&src);
    sh_store_log(e->st, "installed %s from %s", it->name, it->source);
}

static void install_task(job *j, repo_slot *s, const group *gr) {
    install_item *items = j->items;
    if (!ensure_synced(j->e, s)) {
        for (size_t i = 0; i < gr->n; ++i) items[gr->items[i]].why = SH_IMPORT_UNREACHABLE;
        return;
    }
    placement *p = sh_xcalloc(gr->n + 1, sizeof *p);
    for (size_t i = 0; i < gr->n; ++i) {
        install_item *it = &items[gr->items[i]];
        p[i].name = it->name;
        p[i].dir = sh_strdup(it->dir);
        if (it->find_if_moved && !*sh_repo_id(&s->repo, p[i].dir)) {
            const char *moved = find_skill(s, it->name);
            if (moved) {
                free(p[i].dir);
                p[i].dir = sh_dir_of(moved);
            } else {
                it->why = SH_IMPORT_GONE;
            }
        }
        p[i].hash = sh_strdup(it->why == SH_IMPORT_GONE ? "" : sh_repo_id(&s->repo, p[i].dir));
    }
    place_all(j->e, s, p, gr->n);
    for (size_t i = 0; i < gr->n; ++i) {
        install_item *it = &items[gr->items[i]];
        it->done = p[i].ok;
        if (p[i].ok) installed(j->e, s, it, p[i].dir, p[i].hash);
        else if (it->why == SH_IMPORT_OK) it->why = SH_IMPORT_FAILED;
    }
    placements_free(p, gr->n);
}

static int install_items(sh_engine *e, install_item *items, const char *const *slugs, const char *const *urls, size_t n) {
    job j = {e, {NULL, 0}, install_task, items};
    for (size_t i = 0; i < n; ++i) group_add(&j.g, slot_for(e, slugs[i], urls[i]), i);
    run_job(&j);
    groups_free(&j.g);
    int ok = 0;
    for (size_t i = 0; i < n; ++i) ok += items[i].done;
    if (ok) sh_store_relink(e->st);
    return ok;
}

int sh_engine_install(sh_engine *e, const sh_candidate *const *cands, size_t n, bool *done) {
    install_item *items = sh_xcalloc(n + 1, sizeof *items);
    const char **slugs = sh_xcalloc(n + 1, sizeof *slugs), **urls = sh_xcalloc(n + 1, sizeof *urls);
    for (size_t i = 0; i < n; ++i) {
        items[i] = (install_item){cands[i]->name, cands[i]->dir, false, cands[i]->slug, false, SH_IMPORT_OK};
        slugs[i] = cands[i]->slug;
        urls[i] = cands[i]->url;
    }
    const int ok = install_items(e, items, slugs, urls, n);
    for (size_t i = 0; i < n; ++i) done[i] = items[i].done;
    free(slugs);
    free(urls);
    free(items);
    return ok;
}

int sh_engine_import(sh_engine *e, const sh_import_item *const *in, size_t n, bool *done, sh_import_why *why, char **errs) {
    install_item *items = sh_xcalloc(n + 1, sizeof *items);
    char **dirs = sh_xcalloc(n + 1, sizeof *dirs), **slugs = sh_xcalloc(n + 1, sizeof *slugs), **urls = sh_xcalloc(n + 1, sizeof *urls);
    for (size_t i = 0; i < n; ++i) {
        slugs[i] = sh_slug_from_input(in[i]->source, &urls[i]);  // checked by the caller: a plain owner/repo
        dirs[i] = sh_dir_of(in[i]->skill_path);
        items[i] = (install_item){in[i]->name, dirs[i], true, slugs[i], false, SH_IMPORT_OK};
    }
    const int ok = install_items(e, items, (const char *const *)slugs, (const char *const *)urls, n);
    for (size_t i = 0; i < n; ++i) {
        done[i] = items[i].done;
        why[i] = items[i].why;
        errs[i] = NULL;
        if (why[i] == SH_IMPORT_UNREACHABLE) {
            const size_t at = slot_for(e, slugs[i], urls[i]);  // before e->slots is read: slot_for may move it
            if (e->slots[at].err && *e->slots[at].err) errs[i] = sh_strdup(e->slots[at].err);
        }
        free(dirs[i]);
        free(slugs[i]);
        free(urls[i]);
    }
    free(dirs);
    free(slugs);
    free(urls);
    free(items);
    return ok;
}

// ---- track ----

bool sh_store_track(sh_store *st, const char *name, const char *repo_input, const char *path, char **err) {
    char *url = NULL;
    char *slug = sh_slug_from_input(repo_input, &url);
    if (!slug) {
        if (err) *err = sh_strdup("expected owner/repo or a github.com URL");
        return false;
    }
    char *md = sh_strfmt("%s/%s/SKILL.md", st->central, name);
    const bool have = sh_exists(md);
    free(md);
    if (!have) {
        if (err) *err = sh_strfmt("no such skill: %s", name);
        free(slug);
        free(url);
        return false;
    }
    sh_engine *e = sh_engine_new(st);
    const size_t at = slot_for(e, slug, url);  // before e->slots is read: slot_for may move it
    repo_slot *s = &e->slots[at];
    bool ok = ensure_synced(e, s);
    char *skill_md = NULL;
    if (!ok) {
        if (err) *err = sh_strdup(s->err ? s->err : "");
    } else if (path && *path) {
        skill_md = sh_ends_with(path, "SKILL.md") ? sh_strdup(path) : sh_strfmt("%s/SKILL.md", path);
    } else {
        const char *found = find_skill(s, name);
        skill_md = sh_strdup(found ? found : "");
    }
    if (ok && !*skill_md) {
        if (err) *err = sh_strfmt("skill %s not found in %s", name, slug);
        ok = false;
    } else if (ok) {
        sh_source src = {0};
        src.source = sh_strdup(slug);
        src.url = sh_strdup(url);
        src.skill_path = sh_strdup(skill_md);
        src.hash = sh_strdup("");  // the content of the local copy is unknown: the first update syncs it
        src.installed_at = sh_now_iso();
        src.updated_at = sh_strdup(src.installed_at);
        sh_lock_put(st, name, &src);
        sh_source_free(&src);
        sh_store_log(st, "tracking %s <- %s (%s)", name, slug, skill_md);
    }
    free(skill_md);
    sh_engine_free(e);
    free(slug);
    free(url);
    return ok;
}
