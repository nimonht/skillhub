#include "sh_engine.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sh_collate.h"
#include "sh_frontmatter.h"
#include "sh_fs.h"
#include "sh_git.h"
#include "sh_history.h"
#include "sh_json.h"
#include "sh_origin.h"
#include "sh_path.h"
#include "sh_state.h"
#include "sh_thread.h"
#include "sh_time.h"
#include "sh_utf.h"

#define WORKERS 16          // repositories handled at once; each runs one git process at a time
#define FETCHES_PER_HOST 8  // clones and fetches from one host at a time
#define COMMIT_EVERY 200    // lock entries per group commit ...
#define COMMIT_MS 5000      // ... or after this long, whichever comes first
#define DISCOVER_BUDGET 400 // SKILL.md files looked at per repository when listing what it offers, as in 1.x
#define FRONTMATTER_MAX (64 * 1024)
#define WALK_MAX 200000     // files searched for skills in a local folder: more than this is no folder of skills

// ---- repositories ----

typedef struct {
    char *path;  // of SKILL.md
    char *name;  // from its front matter ("" when it has none)
    char *description;
} skill_file;

// A place skills come from: a cache clone, or a local folder read directly.
typedef struct {
    sh_origin o;
    char *key;          // what makes it this repository: the cache folder, or the local folder
    bool tried, ok;
    char *err;          // why it could not be brought up to date, cut like 1.x cut it
    sh_repo repo;       // git
    sh_strv local;      // local folder: every SKILL.md below it
    bool scanned;       // `skills` read
    skill_file *skills; // every SKILL.md there, best location first
    size_t n_skills;
} repo_slot;

typedef struct {
    char *name;
    sh_source src;
    char *tree;     // the tree id to remember (GitLab and git sources), or ""
    char *content;  // the SHA-256 folder hash of what was placed
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
    sh_trees *trees;  // what GitLab and git skills were installed from; read-only while tasks run
    int workers;      // repositories handled at once
    bool auto_mode;
    bool skip_scripts;
    long cancelled;  // read and written through sh_atomic_*
    sh_engine_progress progress;
    void *progress_user;
};

void sh_engine_cancel(sh_engine *e) { sh_atomic_inc(&e->cancelled); }
bool sh_engine_cancelled(sh_engine *e) { return sh_atomic_get(&e->cancelled) > 0; }

static bool setting_skip_scripts(void) {
    char *config = sh_config_dir();
    char *path = sh_path_join(config, "settings.json");
    yyjson_doc *doc = NULL;
    bool skip = true;
    if (sh_json_read(path, 0, &doc) == SH_JSON_OK) {
        yyjson_val *updates = yyjson_obj_get(yyjson_doc_get_root(doc), "updates");
        yyjson_val *value = updates ? yyjson_obj_get(updates, "autoSkipScripts") : NULL;
        if (yyjson_is_bool(value)) skip = yyjson_get_bool(value);
        yyjson_doc_free(doc);
    }
    free(path);
    free(config);
    return skip;
}

void sh_engine_set_auto(sh_engine *e) {
    e->auto_mode = true;
    e->skip_scripts = setting_skip_scripts();
}

void sh_engine_on_progress(sh_engine *e, sh_engine_progress fn, void *user) {
    e->progress = fn;
    e->progress_user = user;
}

sh_engine *sh_engine_new(sh_store *st) {
    sh_engine *e = sh_xcalloc(1, sizeof *e);
    e->st = st;
    e->workers = (int)sh_setting_int("updates", "parallel", WORKERS);
    if (e->workers < 1) e->workers = 1;
    if (e->workers > 64) e->workers = 64;
    const long long per_host = sh_setting_int("updates", "parallelPerHost", FETCHES_PER_HOST);
    e->fetches = sh_limiter_new(per_host < 1 ? 1 : per_host > 64 ? 64 : (int)per_host);
    e->m = sh_mutex_new();
    e->last_commit = sh_mono_ms();
    return e;
}

static void slot_free(repo_slot *s) {
    sh_origin_free(&s->o);
    free(s->key);
    free(s->err);
    sh_repo_free(&s->repo);
    sh_strv_free(&s->local);
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
        free(e->pending[i].tree);
        free(e->pending[i].content);
        sh_source_free(&e->pending[i].src);
    }
    free(e->pending);
    sh_trees_free(e->trees);
    sh_limiter_free(e->fetches);
    sh_mutex_free(e->m);
    free(e);
}

static bool is_local(const repo_slot *s) { return s->o.type == SH_ORIGIN_LOCAL; }

// the lock says nothing about a tree: GitLab and git entries and everything in a project hold a hash of the files
static bool remembers_tree(const sh_engine *e, const repo_slot *s) {
    return !is_local(s) && (e->st->project || s->o.type == SH_ORIGIN_GITLAB || s->o.type == SH_ORIGIN_GIT);
}

// the slot of a repository, added on first use (calling thread only, before any task runs)
static size_t slot_for(sh_engine *e, const sh_origin *o) {
    char *key = o->type == SH_ORIGIN_LOCAL ? sh_strdup(o->url) : sh_origin_cache_dir(o, e->st->cache);
    for (size_t i = 0; i < e->n_slots; ++i) {
        if (strcmp(e->slots[i].key, key) == 0) {
            free(key);
            return i;
        }
    }
    if (e->n_slots == e->cap_slots) {
        e->cap_slots = e->cap_slots ? e->cap_slots * 2 : 16;
        e->slots = sh_xrealloc(e->slots, e->cap_slots * sizeof *e->slots);
    }
    repo_slot *s = &e->slots[e->n_slots];
    memset(s, 0, sizeof *s);
    s->key = key;
    s->o.type = o->type;
    s->o.source = sh_strdup(o->source);
    s->o.url = sh_strdup(o->url);
    s->o.ref = sh_strdup(o->ref);
    s->o.subpath = sh_strdup("");  // a subpath belongs to one input, not to the repository
    s->o.host = sh_strdup(o->host);
    return e->n_slots++;
}

static ptrdiff_t slot_with_key(const sh_engine *e, const char *key) {
    for (size_t i = 0; i < e->n_slots; ++i)
        if (strcmp(e->slots[i].key, key) == 0) return (ptrdiff_t)i;
    return -1;
}

// A project's lock came with the repository, so what it declares proves nothing: what counts is where the source leads. A
// folder has to be inside the project once links are followed, and GitHub has to be GitHub, the repository the entry names.
static bool project_source_ok(const sh_store *st, const sh_source *src, sh_origin_type declared, const sh_origin *o) {
    if (o->type == SH_ORIGIN_LOCAL) {
        char *real = sh_canonical(o->url);
        char *inside = sh_strfmt("%s/", st->project);
        const bool ok = declared == SH_ORIGIN_LOCAL && real && sh_path_within(real, inside);
        free(inside);
        free(real);
        return ok;
    }
    if (declared == SH_ORIGIN_LOCAL) return false;
    return declared != SH_ORIGIN_GITHUB || (o->type == SH_ORIGIN_GITHUB && strcmp(o->source, src->source) == 0);
}

// the origin of a lock entry whose type is one Skill Hub runs; false when the entry cannot be used
static bool origin_of(const sh_store *st, const sh_source *src, sh_origin *o) {
    memset(o, 0, sizeof *o);
    if (src->ref && !sh_origin_ref_ok(src->ref)) return false;
    const char *ref = src->ref ? src->ref : "";
    const bool github = !src->type || !*src->type || strcmp(src->type, "github") == 0;
    if (github && !st->project) {
        o->type = SH_ORIGIN_GITHUB;
        o->source = sh_strdup(src->source);
        o->url = *src->url ? sh_strdup(src->url) : sh_strfmt("https://github.com/%s.git", src->source);
        o->ref = sh_strdup(ref);
        o->subpath = sh_strdup("");
        o->host = sh_strdup("github.com");
        return true;
    }
    sh_origin_type type = SH_ORIGIN_GITHUB;
    if (!github && !sh_origin_type_from_name(src->type, &type)) return false;
    if (!sh_origin_parse(*src->url ? src->url : src->source, o, NULL)) return false;
    if (st->project && !project_source_ok(st, src, type, o)) {
        sh_origin_free(o);
        return false;
    }
    o->type = type;  // what the entry says, not what the URL looks like
    free(o->source);
    free(o->ref);
    o->source = sh_strdup(src->source);
    o->ref = sh_strdup(ref);
    return true;
}

typedef struct {
    sh_strv *found;
    size_t seen;
} skill_walk;

static bool walk_skills(const char *rel, const char *full, bool is_link, void *user) {
    skill_walk *w = user;
    (void)full;
    if (++w->seen > WALK_MAX) return false;
    if (!is_link && (strcmp(rel, "SKILL.md") == 0 || sh_ends_with(rel, "/SKILL.md"))) sh_strv_push(w->found, rel);
    return true;
}

static bool open_local(repo_slot *s, char **err) {
    if (!sh_is_dir(s->o.url)) {
        *err = sh_strfmt("no such folder: %s", s->o.url);
        return false;
    }
    skill_walk w = {&s->local, 0};
    sh_walk_files(s->o.url, walk_skills, &w);
    if (w.seen > WALK_MAX) {
        *err = sh_strfmt("%s holds too many files to search; choose the folder of the skills", s->o.url);
        return false;
    }
    return true;
}

// git asks for no password here (GIT_TERMINAL_PROMPT=0), so a private repository fails: say what to set up
static const char *login_hint(const char *err) {
    static const char *const signs[] = {"terminal prompts disabled", "could not read Username", "Authentication failed", "Permission denied (publickey)",
                                        "Host key verification failed"};
    for (size_t i = 0; err && i < sizeof signs / sizeof signs[0]; ++i)
        if (strstr(err, signs[i])) return " (this repository needs a login: set up a git credential helper for https, or an SSH key, then try again)";
    return "";
}

// brings a repository up to date once per engine; only the task that owns the slot calls this
static bool ensure_synced(sh_engine *e, repo_slot *s) {
    if (s->tried) return s->ok;
    s->tried = true;
    char *err = NULL;
    if (is_local(s)) {
        s->ok = open_local(s, &err);
    } else {
        if (!sh_repo_cloned(s->key)) sh_store_log(e->st, "cloning %s ...", s->o.source);
        s->ok = sh_repo_open(&s->repo, s->key, s->o.url, s->o.ref, e->fetches, &err);
    }
    if (!s->ok) {
        const char *hint = login_hint(err);
        char *shown = sh_left_utf16(err ? err : "", 200);
        sh_store_log(e->st, is_local(s) ? "cannot read %s: %s" : "git failed for %s: %s%s", s->o.source, shown, hint);
        free(shown);
        char *brief = sh_left_utf16(err ? err : "", 300);
        s->err = sh_strfmt("%s%s", brief, hint);
        free(brief);
    }
    free(err);
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
    const char *path;
    const char *id;  // the blob, in git
} ranked_entry;

static int cmp_ranked(const void *a, const void *b) {
    const ranked_entry *x = a, *y = b;
    if (x->rank != y->rank) return x->rank < y->rank ? -1 : 1;
    return strcmp(x->path, y->path);
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

static size_t rank_skill_files(const repo_slot *s, ranked_entry *ranked) {
    size_t n = 0;
    if (is_local(s)) {
        for (size_t i = 0; i < s->local.len; ++i) {
            const int rank = path_rank(s->local.items[i]);
            if (rank != NEVER) ranked[n++] = (ranked_entry){rank, s->local.items[i], NULL};
        }
    } else {
        for (size_t i = 0; i < s->repo.n; ++i) {
            const sh_tree_entry *t = &s->repo.entries[i];
            if (t->mode == SH_MODE_TREE || (strcmp(t->path, "SKILL.md") != 0 && !sh_ends_with(t->path, "/SKILL.md"))) continue;
            const int rank = path_rank(t->path);
            if (rank != NEVER) ranked[n++] = (ranked_entry){rank, t->path, t->id};
        }
    }
    qsort(ranked, n, sizeof *ranked, cmp_ranked);
    return n;
}

// a file or folder of a local source
static char *local_path(const repo_slot *s, const char *rel) { return *rel ? sh_path_join(s->o.url, rel) : sh_strdup(s->o.url); }

// every SKILL.md there, best location first, with the front matter of each (in git, read through one process)
static void scan_skills(repo_slot *s) {
    if (s->scanned) return;
    s->scanned = true;
    ranked_entry *ranked = sh_xcalloc((is_local(s) ? s->local.len : s->repo.n) + 1, sizeof *ranked);
    const size_t n = rank_skill_files(s, ranked);
    s->skills = sh_xcalloc(n + 1, sizeof *s->skills);
    s->n_skills = n;
    const char **ids = sh_xcalloc(n + 1, sizeof *ids);
    for (size_t i = 0; i < n; ++i) {
        s->skills[i].path = sh_strdup(ranked[i].path);
        ids[i] = ranked[i].id;
    }
    if (is_local(s)) {
        for (size_t i = 0; i < n; ++i) {
            char *md = local_path(s, s->skills[i].path);
            sh_frontmatter fm;
            sh_fm_read(md, &fm);
            s->skills[i].name = sh_strdup(sh_fm_value(&fm, "name"));
            s->skills[i].description = sh_strdup(sh_fm_value(&fm, "description"));
            sh_fm_free(&fm);
            free(md);
        }
    } else {
        fm_reader fr = {s->skills, SH_BUF_INIT};
        const sh_blob_reader rd = {fm_begin, fm_data, fm_end, &fr};
        sh_repo_read(&s->repo, ids, n, &rd, NULL);  // a file that could not be read has no front matter, as in 1.x
        sh_buf_free(&fr.text);
    }
    for (size_t i = 0; i < n; ++i) {  // ... and whatever was not reached at all
        if (!s->skills[i].name) s->skills[i].name = sh_strdup("");
        if (!s->skills[i].description) s->skills[i].description = sh_strdup("");
    }
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

// what says a folder's content changed: its git tree id, or the folder hash of a local folder; "" when it is not there
static char *upstream_id(const repo_slot *s, const char *dir) {
    if (!is_local(s)) return sh_strdup(sh_repo_id(&s->repo, dir));
    char *folder = local_path(s, dir);
    char *id = sh_is_dir(folder) ? sh_folder_hash(folder) : sh_strdup("");
    free(folder);
    return id;
}

static bool upstream_has(const repo_slot *s, const char *path) {
    if (!is_local(s)) return sh_repo_find(&s->repo, path) != NULL;
    char *full = local_path(s, path);
    const bool yes = sh_exists(full);
    free(full);
    return yes;
}

// ---- group commits to the lock file ----

static void queue_entry(sh_engine *e, const char *name, const sh_source *src, const char *tree, const char *content) {
    sh_mutex_lock(e->m);
    if (e->n_pending == e->cap_pending) {
        e->cap_pending = e->cap_pending ? e->cap_pending * 2 : 64;
        e->pending = sh_xrealloc(e->pending, e->cap_pending * sizeof *e->pending);
    }
    pending_entry *p = &e->pending[e->n_pending++];
    p->name = sh_strdup(name);
    p->tree = sh_strdup(tree);
    p->content = sh_strdup(content);
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
    const char **names = sh_xcalloc(n, sizeof *names), **trees = sh_xcalloc(n, sizeof *trees), **contents = sh_xcalloc(n, sizeof *contents);
    const sh_source **srcs = sh_xcalloc(n, sizeof *srcs);
    for (size_t i = 0; i < n; ++i) {
        names[i] = batch[i].name;
        srcs[i] = &batch[i].src;
        trees[i] = batch[i].tree;
        contents[i] = batch[i].content;
    }
    sh_lock_put_many(e->st, names, srcs, n);
    sh_trees_put_many(e->st, names, trees, contents, n);
    for (size_t i = 0; i < n; ++i) {
        free(batch[i].name);
        free(batch[i].tree);
        free(batch[i].content);
        sh_source_free(&batch[i].src);
    }
    free(names);
    free(trees);
    free(contents);
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
    long done;   // skills through, for progress (sh_atomic_*)
    long total;
};

static void run_task(size_t i, void *arg) {
    job *j = arg;
    const group *gr = &j->g.groups[i];
    if (sh_engine_cancelled(j->e)) return;  // a safe point: nothing of this repository has been touched yet
    j->task(j, &j->e->slots[gr->slot], gr);
    const long done = sh_atomic_add(&j->done, (long)gr->n);
    if (j->e->progress) j->e->progress(j->e->progress_user, (size_t)done, (size_t)j->total, j->e->slots[gr->slot].o.source);
}

static void on_idle(void *arg) { maybe_commit(((job *)arg)->e); }

static void run_job(job *j) {
    for (size_t i = 0; i < j->g.n; ++i) j->total += (long)j->g.groups[i].n;
    const int workers = j->g.n < (size_t)j->e->workers ? (int)j->g.n : j->e->workers;
    sh_parallel_for(j->g.n, workers, run_task, j, on_idle, 250);
    commit_pending(j->e);
}

static void set_str(char **field, const char *value) {
    free(*field);
    *field = sh_strdup(value ? value : "");
}

// ---- check ----

static void check_skill(sh_engine *e, repo_slot *s, sh_skill *sk) {
    if (sk->live) {  // a link to the folder: always what the folder holds
        sk->update = SH_UPDATE_CURRENT;
        return;
    }
    char *dir = sh_dir_of(sk->src.skill_path);
    char *id = *sk->src.skill_path ? upstream_id(s, dir) : sh_strdup("");
    if (!*id || !upstream_has(s, sk->src.skill_path)) {  // upstream moved the folder: look it up by skill name
        const char *found = find_skill(s, sk->name);
        free(dir);
        free(id);
        if (!found) {
            sk->update = SH_UPDATE_ERROR;
            set_str(&sk->note, "not found upstream");
            return;
        }
        set_str(&sk->new_path, found);
        dir = sh_dir_of(found);
        id = upstream_id(s, dir);
    }
    set_str(&sk->new_hash, id);
    // what the installed copy came from: the lock says it, except that for GitLab and git it holds a hash of the files,
    // which says nothing about a tree; an empty hash means the content is unknown, whatever was recorded
    const char *had = remembers_tree(e, s) && *sk->src.hash ? sh_trees_get(e->trees, sk->name) : sk->src.hash;
    // the same files elsewhere upstream are worth an update, for the path; a local folder has no path on record in a project
    sk->update = strcmp(id, had) == 0 && (!*sk->new_path || is_local(s)) ? SH_UPDATE_CURRENT : SH_UPDATE_AVAILABLE;
    free(id);
    free(dir);
}

static void check_task(job *j, repo_slot *s, const group *gr) {
    sh_skill *skills = j->items;
    const bool ok = ensure_synced(j->e, s);
    for (size_t i = 0; i < gr->n; ++i) {
        sh_skill *sk = &skills[gr->items[i]];
        if (ok) {
            check_skill(j->e, s, sk);
        } else {
            sk->update = SH_UPDATE_ERROR;
            set_str(&sk->note, s->err);
        }
    }
}

int sh_engine_check(sh_engine *e, sh_skills *skills, const sh_strv *only) {
    job j = {e, {NULL, 0}, check_task, skills->items, 0, 0};
    sh_trees_free(e->trees);
    e->trees = sh_trees_load(e->st);
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
        if (!sh_source_runnable(&s->src)) continue;  // managed by npx: a kind of source Skill Hub does not run
        sh_origin o;
        if (!origin_of(e->st, &s->src, &o)) {
            s->update = SH_UPDATE_ERROR;
            set_str(&s->note, "its source cannot be used");
            continue;
        }
        group_add(&j.g, slot_for(e, &o), i);
        sh_origin_free(&o);
    }
    run_job(&j);
    groups_free(&j.g);
    const bool whole = !only || only->len == 0;
    int available = 0;
    for (size_t i = 0; i < skills->len; ++i)
        if (skills->items[i].update == SH_UPDATE_AVAILABLE && (!only || only->len == 0 || sh_strv_contains(only, skills->items[i].name)))
            ++available;
    if (whole) {
        int errors = 0;
        for (size_t i = 0; i < skills->len; ++i) errors += skills->items[i].update == SH_UPDATE_ERROR;
        sh_last_check_put(e->st, available, errors);
    }
    return available;
}

// ---- installing folders from a repository ----

// One skill folder to bring into the shared folder.
typedef struct {
    const char *name;
    char *dir;       // folder inside the repository
    char *id;        // upstream_id of that folder ("" = not there)
    char *stage;
    bool link;       // local folders: a live link instead of a copy
    bool ok;
    bool left_out;   // an automatic update that waits for the user
    sh_backup_note note;
} placement;

typedef struct {
    const char *to;
    size_t files;
    long long bytes;
    char *err;
} copy_walk;

// what extraction from git does, for a local folder or a copy of the installed one: plain files only, with the same limits
static bool copy_one(const char *rel, const char *full, bool is_link, void *user) {
    copy_walk *c = user;
    if (is_link || sh_folder_hash_skips(rel)) return true;
    c->bytes += sh_file_size(full);
    if (++c->files > SH_SKILL_MAX_FILES || c->bytes > SH_SKILL_MAX_BYTES) {
        c->err = sh_strdup(c->files > SH_SKILL_MAX_FILES ? "more than 5000 files" : "more than 128 MB");
        return false;
    }
    char *to = sh_path_join(c->to, rel), *parent = sh_path_parent(to);
    sh_mkpath(parent);
    const bool ok = sh_copy_file(full, to);
    if (!ok) c->err = sh_strfmt("cannot write %s", rel);
    free(parent);
    free(to);
    return ok;
}

static char *copy_folder(const char *from, const char *to) {  // NULL, or why it failed
    sh_remove_tree(to);
    sh_mkpath(to);
    copy_walk c = {to, 0, 0, NULL};
    sh_walk_files(from, copy_one, &c);
    return c.err;
}

// an automatic update that would bring in new scripts waits for the user to look at it
static bool held_back(sh_engine *e, placement *p) {
    if (!e->skip_scripts) return false;
    char *installed = sh_store_skill_dir(e->st, p->name);
    p->left_out = sh_adds_scripts(p->stage, installed);
    free(installed);
    if (p->left_out) sh_store_log(e->st, "left %s out: the update adds or changes scripts (skillhub update %s --review)", p->name, p->name);
    return p->left_out;
}

static void place_local(sh_engine *e, repo_slot *s, placement *p, size_t n) {
    for (size_t i = 0; i < n && !sh_engine_cancelled(e); ++i) {
        if (!*p[i].id) continue;
        char *folder = local_path(s, p[i].dir);
        if (p[i].link) {
            p[i].ok = sh_store_place_link(e->st, p[i].name, folder, &p[i].note);
        } else {
            p[i].stage = sh_store_stage_dir(e->st, p[i].name);
            char *err = copy_folder(folder, p[i].stage);
            if (err) {
                sh_store_log(e->st, "extract failed for %s: %s", p[i].name, err);
                sh_remove_tree(p[i].stage);
                free(err);
            } else if (held_back(e, &p[i])) {
                sh_remove_tree(p[i].stage);
            } else {
                p[i].ok = sh_store_place(e->st, p[i].name, p[i].stage, &p[i].note);
            }
        }
        free(folder);
    }
}

// Extracts every placement through one git process, then swaps each into the shared folder on its own.
static void place_all(sh_engine *e, repo_slot *s, placement *p, size_t n) {
    if (is_local(s)) {
        place_local(e, s, p, n);
        return;
    }
    sh_extract *x = sh_xcalloc(n + 1, sizeof *x);
    size_t *which = sh_xcalloc(n + 1, sizeof *which);
    size_t k = 0;
    for (size_t i = 0; i < n; ++i) {
        p[i].ok = false;
        if (!*p[i].id) continue;
        p[i].stage = sh_store_stage_dir(e->st, p[i].name);
        sh_remove_tree(p[i].stage);
        sh_mkpath(p[i].stage);
        x[k] = (sh_extract){p[i].dir, p[i].stage, false, NULL};
        which[k++] = i;
    }
    sh_repo_extract(&s->repo, x, k);
    for (size_t m = 0; m < k; ++m) {
        placement *pl = &p[which[m]];
        if (sh_engine_cancelled(e)) {  // a safe point: between two swaps
            sh_remove_tree(pl->stage);
        } else if (!x[m].ok) {
            sh_store_log(e->st, "extract failed for %s: %s", pl->name, x[m].err ? x[m].err : "");
            sh_remove_tree(pl->stage);
        } else if (held_back(e, pl)) {
            sh_remove_tree(pl->stage);
        } else {
            pl->ok = sh_store_place(e->st, pl->name, pl->stage, &pl->note);
        }
        free(x[m].err);
    }
    free(which);
    free(x);
}

static void placements_free(placement *p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        free(p[i].dir);
        free(p[i].id);
        free(p[i].stage);
    }
    free(p);
}

static char *placed_hash(const sh_engine *e, const char *name) {
    char *folder = sh_store_skill_dir(e->st, name);
    char *hash = sh_folder_hash(folder);
    free(folder);
    return hash;
}

// what the lock holds for a placed skill, as npx writes it: the git tree id for GitHub, the SHA-256 folder hash otherwise
static const char *lock_hash(const sh_engine *e, const repo_slot *s, const char *content, const char *id) {
    return s->o.type == SH_ORIGIN_GITHUB && !e->st->project ? id : content;
}

// the upstream tree id the installed skill came from
static const char *installed_tree(const sh_engine *e, const repo_slot *s, const sh_skill *sk) {
    return remembers_tree(e, s) ? sh_trees_get(e->trees, sk->name) : sk->src.hash;
}

// ---- update ----

typedef struct {
    sh_skill *const *todo;
    bool *done;
} update_items;

static void updated(sh_engine *e, const repo_slot *s, sh_skill *sk, const char *skill_md, const char *id) {
    sh_source src;
    sh_source_copy(&sk->src, &src);
    if (!*src.url) set_str(&src.url, s->o.url);
    set_str(&src.skill_path, skill_md);
    char *content = placed_hash(e, sk->name);
    set_str(&src.hash, lock_hash(e, s, content, id));
    char *now = sh_now_iso();
    if (!*src.installed_at) set_str(&src.installed_at, now);
    set_str(&src.updated_at, now);
    free(now);
    queue_entry(e, sk->name, &src, remembers_tree(e, s) ? id : "", content);
    free(content);
    sh_source_free(&sk->src);
    sk->src = src;
    sk->update = SH_UPDATE_CURRENT;
    set_str(&sk->new_hash, "");
    set_str(&sk->new_path, "");
    char *dir = sh_store_skill_dir(e->st, sk->name);
    char *md = sh_path_join(dir, "SKILL.md");
    free(dir);
    sh_frontmatter fm;
    sh_fm_read(md, &fm);
    set_str(&sk->description, sh_fm_value(&fm, "description"));
    sh_fm_free(&fm);
    free(md);
    sh_store_log(e->st, "updated %s -> %.8s", sk->name, id);
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
        p[i].id = *sk->new_hash ? sh_strdup(sk->new_hash) : upstream_id(s, p[i].dir);
        p[i].note = (sh_backup_note){"update", &sk->src, installed_tree(j->e, s, sk)};
    }
    place_all(j->e, s, p, gr->n);
    for (size_t i = 0; i < gr->n; ++i) {
        it->done[gr->items[i]] = p[i].ok;
        it->todo[gr->items[i]]->left_out = p[i].left_out;
        if (p[i].ok) updated(j->e, s, it->todo[gr->items[i]], skill_mds[i], p[i].id);
        free(skill_mds[i]);
    }
    free(skill_mds);
    placements_free(p, gr->n);
}

// a rollback holds a skill for as long as upstream stays at the version rolled back from; an update ends it
static bool on_hold(sh_engine *e, const sh_holds *holds, sh_skill *sk) {
    const char *from = sh_holds_get(holds, sk->name);
    if (!from || (*from && strcmp(from, sk->new_hash) != 0)) return false;
    sh_store_log(e->st, "left %s out: it is on hold (skillhub release %s)", sk->name, sk->name);
    sk->left_out = true;
    return true;
}

static void release_holds(sh_engine *e, sh_skill *const *todo, size_t n, const bool *done) {
    const char **names = sh_xcalloc(n + 1, sizeof *names);
    size_t k = 0;
    for (size_t i = 0; i < n; ++i)
        if (done[i]) names[k++] = todo[i]->name;
    sh_holds_release(e->st, names, k);
    free(names);
}

int sh_engine_update(sh_engine *e, sh_skill *const *todo, size_t n, bool *done) {
    update_items it = {todo, done};
    job j = {e, {NULL, 0}, update_task, &it, 0, 0};
    sh_holds *holds = e->auto_mode ? sh_holds_load(e->st) : NULL;
    for (size_t i = 0; i < n; ++i) {
        done[i] = false;
        sh_origin o;
        if (!sh_source_runnable(&todo[i]->src) || todo[i]->live || on_hold(e, holds, todo[i]) || !origin_of(e->st, &todo[i]->src, &o)) continue;
        group_add(&j.g, slot_for(e, &o), i);
        sh_origin_free(&o);
    }
    sh_holds_free(holds);
    run_job(&j);
    groups_free(&j.g);
    int ok = 0;
    for (size_t i = 0; i < n; ++i) ok += done[i];
    if (ok) {
        release_holds(e, todo, n, done);
        sh_store_relink(e->st);
    }
    return ok;
}

// ---- review ----

typedef struct {
    sh_skill *const *todo;
    sh_review *out;
} review_items;

static bool edited_since_placed(const sh_engine *e, const repo_slot *s, const sh_skill *sk) {
    const char *placed = sh_trees_content(e->trees, sk->name);
    if (!*placed && (s->o.type != SH_ORIGIN_GITHUB || e->st->project)) placed = sk->src.hash;  // npx's own hash of the files
    if (!*placed) return false;
    char *now = placed_hash(e, sk->name);
    const bool edited = strcmp(now, placed) != 0;
    free(now);
    return edited;
}

static void write_manifest(const char *stage, const char *name, const char *tree, const char *skill_md, const char *hash) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    sh_json_set_str(d, root, "name", name);
    sh_json_set_str(d, root, "tree", tree);
    sh_json_set_str(d, root, "skillPath", skill_md);
    sh_json_set_str(d, root, "hash", hash);
    char *path = sh_path_join(stage, "review.json");
    sh_json_write(path, d, true, true);
    free(path);
    yyjson_mut_doc_free(d);
}

// the upstream version sits in <stage>/new: compares it with a copy of the installed folder
static void finish_review(sh_engine *e, const repo_slot *s, const sh_skill *sk, sh_review *r, const char *stage, const char *skill_md) {
    char *old = sh_path_join(stage, "old"), *fresh = sh_path_join(stage, "new"), *installed = sh_store_skill_dir(e->st, sk->name);
    r->err = copy_folder(installed, old);
    if (!r->err) sh_review_compare(stage, r);
    sh_remove_tree(old);
    r->available = sk->update == SH_UPDATE_AVAILABLE;
    r->tree = sh_strdup(sk->new_hash);
    r->local_edits = r->n > 0 && (!r->available || edited_since_placed(e, s, sk));
    if (!r->err && r->n > 0) {
        char *hash = sh_folder_hash(fresh);
        write_manifest(stage, sk->name, sk->new_hash, skill_md, hash);
        free(hash);
        r->stage = sh_strdup(stage);
    } else {
        sh_remove_tree(stage);
    }
    free(installed);
    free(fresh);
    free(old);
}

static void review_task(job *j, repo_slot *s, const group *gr) {
    review_items *it = j->items;
    sh_engine *e = j->e;
    const bool synced = ensure_synced(e, s);
    sh_extract *x = sh_xcalloc(gr->n + 1, sizeof *x);
    size_t *which = sh_xcalloc(gr->n + 1, sizeof *which);
    char **stages = sh_xcalloc(gr->n + 1, sizeof *stages), **dirs = sh_xcalloc(gr->n + 1, sizeof *dirs), **mds = sh_xcalloc(gr->n + 1, sizeof *mds);
    size_t k = 0;
    for (size_t i = 0; i < gr->n; ++i) {
        sh_skill *sk = it->todo[gr->items[i]];
        sh_review *r = &it->out[gr->items[i]];
        if (!synced) {
            r->err = sh_strdup(s->err ? s->err : "");
            continue;
        }
        check_skill(e, s, sk);
        if (sk->update == SH_UPDATE_ERROR) {
            r->err = sh_strdup(sk->note);
            continue;
        }
        char *label = sh_strfmt("review-%s", sk->name);
        stages[k] = sh_store_stage_dir(e->st, label);
        free(label);
        mds[k] = sh_strdup(*sk->new_path ? sk->new_path : sk->src.skill_path);
        dirs[k] = sh_dir_of(mds[k]);
        char *fresh = sh_path_join(stages[k], "new");
        sh_mkpath(fresh);
        x[k] = (sh_extract){dirs[k], fresh, false, NULL};
        which[k++] = gr->items[i];
    }
    if (is_local(s)) {
        for (size_t m = 0; m < k; ++m) {
            char *folder = local_path(s, dirs[m]);
            x[m].err = copy_folder(folder, x[m].dest);
            x[m].ok = !x[m].err;
            free(folder);
        }
    } else {
        sh_repo_extract(&s->repo, x, k);
    }
    for (size_t m = 0; m < k; ++m) {
        sh_review *r = &it->out[which[m]];
        if (x[m].ok) {
            finish_review(e, s, it->todo[which[m]], r, stages[m], mds[m]);
        } else {
            r->err = sh_strdup(x[m].err ? x[m].err : "cannot read upstream");
            sh_remove_tree(stages[m]);
        }
        free(x[m].err);
        free((char *)x[m].dest);
        free(stages[m]);
        free(dirs[m]);
        free(mds[m]);
    }
    free(mds);
    free(dirs);
    free(stages);
    free(which);
    free(x);
}

void sh_engine_diff(sh_engine *e, sh_skill *const *todo, size_t n, sh_review *out) {
    review_items it = {todo, out};
    job j = {e, {NULL, 0}, review_task, &it, 0, 0};
    sh_trees_free(e->trees);
    e->trees = sh_trees_load(e->st);
    for (size_t i = 0; i < n; ++i) {
        const sh_skill *sk = todo[i];
        sh_origin o;
        memset(&out[i], 0, sizeof out[i]);
        out[i].name = sh_strdup(sk->name);
        if (!sh_source_tracked(&sk->src)) out[i].err = sh_strfmt("%s has no upstream", sk->name);
        else if (!sh_source_runnable(&sk->src)) out[i].err = sh_strfmt("%s is managed by npx", sk->name);
        else if (sk->live) out[i].err = sh_strfmt("%s is a live link: it always shows what its folder holds", sk->name);
        else if (!origin_of(e->st, &sk->src, &o)) out[i].err = sh_strfmt("%s has a source that cannot be read", sk->name);
        else {
            group_add(&j.g, slot_for(e, &o), i);
            sh_origin_free(&o);
        }
    }
    run_job(&j);
    groups_free(&j.g);
}

static bool apply_one(sh_engine *e, sh_skill *sk, const char *stage) {
    char *root = sh_path_join(e->st->cache, "stage/review-");
    const bool ours = sh_starts_with(stage, root) && !strchr(stage + strlen(root), '/') && !strchr(stage + strlen(root), '\\');
    free(root);
    char *manifest = sh_path_join(stage, "review.json"), *fresh = sh_path_join(stage, "new");
    yyjson_doc *doc = NULL;
    sh_origin o = {0};
    bool ok = false;
    if (!ours || sh_json_read(manifest, 64 * 1024, &doc) != SH_JSON_OK || !origin_of(e->st, &sk->src, &o)) {
        sh_store_log(e->st, "the review of %s is gone: review it again", sk->name);
    } else {
        yyjson_val *root_val = yyjson_doc_get_root(doc);
        char *hash = sh_folder_hash(fresh);
        if (strcmp(sh_json_str(root_val, "name"), sk->name) != 0 || strcmp(sh_json_str(root_val, "hash"), hash) != 0) {
            sh_store_log(e->st, "the review of %s changed on disk: review it again", sk->name);
        } else {
            const size_t at = slot_for(e, &o);  // may move the table
            const repo_slot *s = &e->slots[at];
            const sh_backup_note note = {"update", &sk->src, installed_tree(e, s, sk)};
            ok = sh_store_place(e->st, sk->name, fresh, &note);
            if (ok) updated(e, s, sk, sh_json_str(root_val, "skillPath"), sh_json_str(root_val, "tree"));
        }
        free(hash);
        sh_origin_free(&o);
    }
    if (ours) sh_remove_tree(stage);
    yyjson_doc_free(doc);
    free(fresh);
    free(manifest);
    return ok;
}

int sh_engine_apply(sh_engine *e, sh_skill *const *todo, const char *const *stages, size_t n, bool *done) {
    sh_trees_free(e->trees);
    e->trees = sh_trees_load(e->st);
    int ok = 0;
    for (size_t i = 0; i < n; ++i) {
        done[i] = !sh_engine_cancelled(e) && apply_one(e, todo[i], stages[i]);
        ok += done[i];
    }
    commit_pending(e);
    if (ok) {
        release_holds(e, todo, n, done);
        sh_store_relink(e->st);
    }
    return ok;
}

// ---- discover ----

static void discover_task(job *j, repo_slot *s, const group *gr) {
    (void)gr;
    if (ensure_synced(j->e, s)) scan_skills(s);
}

static bool below(const char *path, const char *subpath) {
    if (!*subpath) return true;
    const size_t n = strlen(subpath);
    return strncmp(path, subpath, n) == 0 && path[n] == '/';
}

static void candidates_of(const repo_slot *s, const char *subpath, sh_discovery *d) {
    sh_strv seen = SH_STRV_INIT;
    d->cands.items = sh_xcalloc(s->n_skills + 1, sizeof *d->cands.items);
    size_t looked = 0;
    for (size_t i = 0; i < s->n_skills && looked < DISCOVER_BUDGET; ++i) {
        const skill_file *f = &s->skills[i];
        if (!below(f->path, subpath)) continue;
        ++looked;
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
        c->slug = sh_strdup(s->o.source);
        c->url = sh_strdup(s->o.url);
        c->key = sh_strdup(s->key);
    }
    if (d->cands.len == 0) d->err = sh_strfmt("no SKILL.md found in %s", s->o.source);
    sh_strv_free(&seen);
}

void sh_engine_discover(sh_engine *e, const char *const *inputs, size_t n, sh_discovery *out) {
    job j = {e, {NULL, 0}, discover_task, NULL, 0, 0};
    size_t *slot_of = sh_xcalloc(n + 1, sizeof *slot_of);
    sh_strv subpaths = SH_STRV_INIT;
    for (size_t i = 0; i < n; ++i) {
        memset(&out[i], 0, sizeof out[i]);
        sh_origin o;
        if (!sh_origin_parse(inputs[i], &o, &out[i].err)) {
            slot_of[i] = SIZE_MAX;
            sh_strv_push(&subpaths, "");
            continue;
        }
        slot_of[i] = slot_for(e, &o);
        group_add(&j.g, slot_of[i], i);
        sh_strv_push(&subpaths, o.subpath);
        sh_origin_free(&o);
    }
    run_job(&j);
    groups_free(&j.g);
    for (size_t i = 0; i < n; ++i) {
        if (slot_of[i] == SIZE_MAX) continue;
        const repo_slot *s = &e->slots[slot_of[i]];
        out[i].type = sh_origin_type_name(s->o.type);
        out[i].host = sh_strdup(s->o.host);
        if (!s->ok) out[i].err = sh_strdup(s->err ? s->err : "");
        else candidates_of(s, subpaths.items[i], &out[i]);
    }
    sh_strv_free(&subpaths);
    free(slot_of);
}

void sh_discovery_free(sh_discovery *d) {
    sh_candidates_free(&d->cands);
    free(d->err);
    free(d->host);
    memset(d, 0, sizeof *d);
}

// ---- install and import ----

// One skill to install: from a candidate, or from an import file (whose folder may have moved upstream).
typedef struct {
    const char *name;
    const char *dir;       // folder inside the repository
    bool find_if_moved;    // import: not at `dir` any more -> look it up by name
    bool link;             // a live link to a local folder
    bool done;
    sh_import_why why;
} install_item;

static void installed(sh_engine *e, const repo_slot *s, const install_item *it, const char *dir, const char *id) {
    sh_source src = {0};
    src.type = sh_strdup(sh_origin_type_name(s->o.type));
    src.ref = sh_strdup(s->o.ref);
    src.source = sh_strdup(s->o.source);
    src.url = sh_strdup(s->o.url);
    src.skill_path = *dir ? sh_strfmt("%s/SKILL.md", dir) : sh_strdup("SKILL.md");
    char *content = placed_hash(e, it->name);
    src.hash = sh_strdup(lock_hash(e, s, content, id));
    src.installed_at = sh_now_iso();
    src.updated_at = sh_strdup(src.installed_at);
    queue_entry(e, it->name, &src, remembers_tree(e, s) ? id : "", content);
    free(content);
    sh_source_free(&src);
    sh_store_log(e->st, it->link ? "linked %s to %s" : "installed %s from %s", it->name, s->o.source);
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
        p[i].link = it->link;
        p[i].note = (sh_backup_note){"install", NULL, ""};
        p[i].dir = sh_strdup(it->dir);
        p[i].id = upstream_id(s, p[i].dir);
        if (it->find_if_moved && !*p[i].id) {
            const char *moved = find_skill(s, it->name);
            if (moved) {
                free(p[i].dir);
                free(p[i].id);
                p[i].dir = sh_dir_of(moved);
                p[i].id = upstream_id(s, p[i].dir);
            } else {
                it->why = SH_IMPORT_GONE;
            }
        }
    }
    place_all(j->e, s, p, gr->n);
    for (size_t i = 0; i < gr->n; ++i) {
        install_item *it = &items[gr->items[i]];
        it->done = p[i].ok;
        if (p[i].ok) installed(j->e, s, it, p[i].dir, p[i].id);
        else if (it->why == SH_IMPORT_OK) it->why = SH_IMPORT_FAILED;
    }
    placements_free(p, gr->n);
}

static int install_items(sh_engine *e, install_item *items, const size_t *slots, size_t n) {
    job j = {e, {NULL, 0}, install_task, items, 0, 0};
    for (size_t i = 0; i < n; ++i)
        if (slots[i] != SIZE_MAX) group_add(&j.g, slots[i], i);
    run_job(&j);
    groups_free(&j.g);
    int ok = 0;
    for (size_t i = 0; i < n; ++i) ok += items[i].done;
    if (ok) sh_store_relink(e->st);
    return ok;
}

int sh_engine_install(sh_engine *e, const sh_candidate *const *cands, size_t n, bool link, bool *done) {
    install_item *items = sh_xcalloc(n + 1, sizeof *items);
    size_t *slots = sh_xcalloc(n + 1, sizeof *slots);
    for (size_t i = 0; i < n; ++i) {
        const ptrdiff_t at = slot_with_key(e, cands[i]->key);  // the engine that discovered it
        slots[i] = at < 0 ? SIZE_MAX : (size_t)at;
        items[i] = (install_item){cands[i]->name, cands[i]->dir, false, link && !e->st->project && at >= 0 && is_local(&e->slots[at]), false, SH_IMPORT_OK};
    }
    const int ok = install_items(e, items, slots, n);
    for (size_t i = 0; i < n; ++i) done[i] = items[i].done;
    free(slots);
    free(items);
    return ok;
}

int sh_engine_import(sh_engine *e, const sh_import_item *const *in, size_t n, bool *done, sh_import_why *why, char **errs) {
    install_item *items = sh_xcalloc(n + 1, sizeof *items);
    size_t *slots = sh_xcalloc(n + 1, sizeof *slots);
    char **dirs = sh_xcalloc(n + 1, sizeof *dirs);
    for (size_t i = 0; i < n; ++i) {
        sh_origin o;
        dirs[i] = sh_dir_of(in[i]->skill_path);
        items[i] = (install_item){in[i]->name, dirs[i], true, false, false, SH_IMPORT_OK};
        slots[i] = SIZE_MAX;
        if (sh_origin_parse(in[i]->source, &o, NULL)) {  // checked by the caller: GitHub or an https URL
            free(o.ref);
            o.ref = sh_strdup(in[i]->ref ? in[i]->ref : "");
            slots[i] = slot_for(e, &o);
            sh_origin_free(&o);
        }
    }
    const int ok = install_items(e, items, slots, n);
    for (size_t i = 0; i < n; ++i) {
        done[i] = items[i].done;
        why[i] = items[i].why;
        errs[i] = NULL;
        if (why[i] == SH_IMPORT_UNREACHABLE && e->slots[slots[i]].err && *e->slots[slots[i]].err) errs[i] = sh_strdup(e->slots[slots[i]].err);
        free(dirs[i]);
    }
    free(dirs);
    free(slots);
    free(items);
    return ok;
}

// ---- copy to a project ----

bool sh_engine_copy_to_project(sh_engine *e, const char *name, const char *project, char **err) {
    sh_store p;
    if (!sh_store_init_project(&p, project, e->st->log, e->st->log_user, err)) return false;
    char *from = sh_store_skill_dir(e->st, name), *to = sh_store_skill_dir(&p, name);
    sh_source src;
    const bool tracked = sh_lock_entry(e->st, name, &src);
    bool ok = false;
    if (!sh_is_dir(from)) {
        if (err) *err = sh_strfmt("no skill named %s", name);
    } else if (sh_lexists(to)) {
        if (err) *err = sh_strfmt("%s is in that project already", name);
    } else {
        char *stage = sh_store_stage_dir(&p, name);
        char *why = copy_folder(from, stage);
        ok = !why && sh_store_place(&p, name, stage, NULL);
        if (why) {
            if (err) *err = why;
            else free(why);
            sh_remove_tree(stage);
        } else if (!ok && err) {
            *err = sh_strfmt("cannot place %s", name);
        }
        free(stage);
    }
    if (ok && tracked && strcmp(src.type, "local") != 0) {  // a folder of this machine means nothing to the team
        sh_trees *t = sh_trees_load(e->st);
        const bool github = !*src.type || strcmp(src.type, "github") == 0;
        char *tree = sh_strdup(github ? src.hash : sh_trees_get(t, name));
        sh_trees_free(t);
        char *content = sh_folder_hash(to);
        free(src.hash);
        src.hash = sh_strdup(content);
        sh_lock_put(&p, name, &src);
        const char *const names[] = {name};
        const char *const trees[] = {tree};
        const char *const contents[] = {content};
        sh_trees_put_many(&p, names, trees, contents, 1);
        free(content);
        free(tree);
    }
    if (ok) sh_store_relink(&p);
    if (tracked) sh_source_free(&src);
    free(to);
    free(from);
    sh_store_free(&p);
    return ok;
}

// ---- track ----

bool sh_store_track(sh_store *st, const char *name, const char *repo_input, const char *path, char **err) {
    sh_origin o;
    if (!sh_origin_parse(repo_input, &o, err)) return false;
    char *dir = sh_store_skill_dir(st, name);
    char *md = sh_path_join(dir, "SKILL.md");
    free(dir);
    const bool have = sh_exists(md);
    free(md);
    if (!have) {
        if (err) *err = sh_strfmt("no such skill: %s", name);
        sh_origin_free(&o);
        return false;
    }
    sh_engine *e = sh_engine_new(st);
    const size_t at = slot_for(e, &o);  // before e->slots is read: slot_for may move it
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
        if (err) *err = sh_strfmt("skill %s not found in %s", name, o.source);
        ok = false;
    } else if (ok) {
        sh_source src = {0};
        src.type = sh_strdup(sh_origin_type_name(o.type));
        src.ref = sh_strdup(o.ref);
        src.source = sh_strdup(o.source);
        src.url = sh_strdup(o.url);
        src.skill_path = sh_strdup(skill_md);
        src.hash = sh_strdup("");  // the content of the local copy is unknown: the first update syncs it
        src.installed_at = sh_now_iso();
        src.updated_at = sh_strdup(src.installed_at);
        sh_lock_put(st, name, &src);
        sh_source_free(&src);
        sh_store_log(st, "tracking %s <- %s (%s)", name, o.source, skill_md);
    }
    free(skill_md);
    sh_engine_free(e);
    sh_origin_free(&o);
    return ok;
}
