// The shared folder: scanning skills and strays, adopting strays, linking agents, and updating or installing skills from
// git. Ported from 1.x src/store.cpp; log messages are the same, word for word, because tests and users read them.
#include "sh_store.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "sh_frontmatter.h"
#include "sh_fs.h"
#include "sh_hash.h"
#include "sh_json.h"
#include "sh_link.h"
#include "sh_path.h"
#include "sh_proc.h"
#include "sh_time.h"
#include "sh_utf.h"

#ifndef _WIN32
#include <strings.h>
#endif

void sh_store_init(sh_store *st, sh_log_fn log, void *user) {
    memset(st, 0, sizeof *st);
    st->log = log;
    st->log_user = user;
    st->home = sh_home();
    st->central = sh_path_join(st->home, ".agents/skills");
    sh_lock_paths_for(st->home, &st->lock_path, &st->lock_other);
    st->backup = sh_path_join(st->home, ".agents/.backup");
    st->cache = sh_cache_dir();
    sh_store_reload_providers(st);
}

void sh_store_free(sh_store *st) {
    free(st->home);
    free(st->central);
    free(st->lock_path);
    free(st->lock_other);
    free(st->backup);
    free(st->cache);
    for (size_t i = 0; i < st->n_providers; ++i) {
        sh_agent *a = &st->providers[i];
        free(a->id); free(a->label); free(a->mode); free(a->dir); free(a->config_file); free(a->detected_by);
        sh_strv_free(&a->extra_dirs); sh_strv_free(&a->ignore); sh_strv_free(&a->bins); sh_strv_free(&a->detect);
    }
    free(st->providers);
    free(st->agents);
    sh_strv_free(&st->fetched);
    memset(st, 0, sizeof *st);
}

void sh_store_log(sh_store *st, const char *fmt, ...) {
    sh_buf b = SH_BUF_INIT;
    va_list ap;
    va_start(ap, fmt);
    sh_buf_vprintf(&b, fmt, ap);
    va_end(ap);
    if (st->log) st->log(st->log_user, sh_buf_str(&b));
    sh_buf_free(&b);
}

static bool hidden_name(const char *n) { return n[0] == '.'; }

char *sh_dir_of(const char *skill_md_path) {
    const char *slash = strrchr(skill_md_path, '/');
    return slash ? sh_strndup(skill_md_path, (size_t)(slash - skill_md_path)) : sh_strdup("");
}

// ---- freeing ----

static void skill_free(sh_skill *s) {
    free(s->name); free(s->description); free(s->path); free(s->links); free(s->new_hash); free(s->new_path); free(s->note);
    sh_source_free(&s->src);
}

void sh_skills_free(sh_skills *s) {
    for (size_t i = 0; i < s->len; ++i) skill_free(&s->items[i]);
    free(s->items);
    s->items = NULL;
    s->len = 0;
}

void sh_strays_free(sh_strays *s) {
    for (size_t i = 0; i < s->len; ++i) {
        free(s->items[i].agent_id);
        free(s->items[i].name);
        free(s->items[i].path);
    }
    free(s->items);
    s->items = NULL;
    s->len = 0;
}

void sh_candidate_free(sh_candidate *c) {
    free(c->name); free(c->description); free(c->dir); free(c->slug); free(c->url);
    memset(c, 0, sizeof *c);
}

void sh_candidates_free(sh_candidates *c) {
    for (size_t i = 0; i < c->len; ++i) sh_candidate_free(&c->items[i]);
    free(c->items);
    c->items = NULL;
    c->len = 0;
}

// ---- scan ----

static sh_link_state state_for(const sh_agent *a, const char *name, const char *central_skill, const char *central) {
    if (strcmp(a->mode, "link") == 0) {
        char *lp = sh_path_join(a->dir, name);
        sh_link_state st;
        if (sh_is_link(lp)) st = sh_exists(lp) && sh_points_to(lp, central_skill) ? SH_LINK_LINKED : SH_LINK_FOREIGN;
        else st = sh_exists(lp) ? SH_LINK_COPY : SH_LINK_MISSING;
        free(lp);
        return st;
    }
    // native / config: the agent reads the shared folder itself; a real folder of the same name in one of its own
    // folders is a stray duplicate
    for (size_t i = 0; i < a->extra_dirs.len; ++i) {
        const char *d = a->extra_dirs.items[i];
        if (strcmp(d, central) == 0) continue;
        char *p = sh_path_join(d, name);
        const bool copy = !sh_is_link(p) && sh_is_dir(p);
        free(p);
        if (copy) return SH_LINK_COPY;
    }
    return SH_LINK_NATIVE;
}

static void fill_links(const sh_store *st, sh_skill *s) {
    s->n_links = st->n_agents;
    s->links = sh_xcalloc(st->n_agents + 1, sizeof *s->links);
    for (size_t k = 0; k < st->n_agents; ++k) s->links[k] = state_for(st->agents[k], s->name, s->path, st->central);
}

void sh_store_scan(const sh_store *st, sh_skills *out) {
    memset(out, 0, sizeof *out);
    yyjson_mut_doc *lock = sh_lock_load(st, NULL);  // unreadable: nothing counts as tracked, and nothing is written
    yyjson_mut_val *lock_skills = lock ? yyjson_mut_obj_get(yyjson_mut_doc_get_root(lock), "skills") : NULL;
    sh_strv names = SH_STRV_INIT;
    sh_list_dir(st->central, SH_LIST_DIRS | SH_LIST_NO_SYMLINKS | SH_SORT_NAME_CI, &names);
    out->items = sh_xcalloc(names.len + 1, sizeof *out->items);
    for (size_t i = 0; i < names.len; ++i) {
        const char *name = names.items[i];
        if (hidden_name(name)) continue;
        char *path = sh_path_join(st->central, name);
        char *md = sh_path_join(path, "SKILL.md");
        if (!sh_exists(md)) {
            free(md);
            free(path);
            continue;
        }
        sh_skill *s = &out->items[out->len++];
        memset(s, 0, sizeof *s);
        s->name = sh_strdup(name);
        s->path = path;
        sh_frontmatter fm;
        sh_fm_read(md, &fm);
        s->description = sh_strdup(sh_fm_value(&fm, "description"));
        sh_fm_free(&fm);
        free(md);
        yyjson_mut_val *entry = lock_skills ? yyjson_mut_obj_get(lock_skills, name) : NULL;
        if (yyjson_mut_is_obj(entry)) {
            yyjson_doc *one = yyjson_mut_val_imut_copy(entry, NULL);
            sh_source_from_json(yyjson_doc_get_root(one), &s->src);
            yyjson_doc_free(one);
        } else {
            sh_source_from_json(NULL, &s->src);
        }
        s->update = sh_source_tracked(&s->src) ? SH_UPDATE_UNKNOWN : SH_UPDATE_UNTRACKED;
        s->new_hash = sh_strdup("");
        s->new_path = sh_strdup("");
        s->note = sh_strdup("");
        fill_links(st, s);
    }
    sh_strv_free(&names);
    yyjson_mut_doc_free(lock);
}

void sh_store_strays(const sh_store *st, sh_strays *out) {
    memset(out, 0, sizeof *out);
    size_t cap = 0;
    sh_strv seen = SH_STRV_INIT;
    for (size_t k = 0; k < st->n_agents; ++k) {
        const sh_agent *a = st->agents[k];
        sh_strv dirs = SH_STRV_INIT;
        if (a->linked) sh_strv_push(&dirs, a->dir);
        for (size_t i = 0; i < a->extra_dirs.len; ++i) sh_strv_push(&dirs, a->extra_dirs.items[i]);
        for (size_t d = 0; d < dirs.len; ++d) {
            const char *dir = dirs.items[d];
            if (!*dir || strcmp(dir, st->central) == 0) continue;
            sh_strv names = SH_STRV_INIT;
            sh_list_dir(dir, SH_LIST_DIRS | SH_LIST_NO_SYMLINKS | SH_SORT_NAME, &names);
            for (size_t i = 0; i < names.len; ++i) {
                const char *n = names.items[i];
                char *path = sh_path_join(dir, n);
                char *md = sh_path_join(path, "SKILL.md");
                char *canonical = NULL;
                const bool candidate = !hidden_name(n) && !sh_strv_contains(&a->ignore, n) && !sh_is_link(path) && sh_exists(md);
                if (candidate) canonical = sh_canonical(path);
                if (candidate && !(canonical && sh_strv_contains(&seen, canonical))) {  // two agents may share a folder
                    if (canonical) sh_strv_push(&seen, canonical);
                    if (out->len == cap) {
                        cap = cap ? cap * 2 : 16;
                        out->items = sh_xrealloc(out->items, cap * sizeof *out->items);
                    }
                    sh_stray *s = &out->items[out->len++];
                    s->agent_id = sh_strdup(a->id);
                    s->name = sh_strdup(n);
                    s->path = sh_strdup(path);
                    char *dest = sh_path_join(st->central, n);
                    s->duplicate = sh_exists(dest);
                    free(dest);
                }
                free(canonical);
                free(md);
                free(path);
            }
            sh_strv_free(&names);
        }
        sh_strv_free(&dirs);
    }
    sh_strv_free(&seen);
}

// ---- adopt / relink ----

// wait until nothing in `dir` changed for a few seconds (an installer may still be writing)
static void wait_quiet(const char *dir) {
    for (int i = 0; i < 30; ++i) {
        const int64_t newest = sh_newest_mtime_ms(dir);
        if ((sh_now_ms() - newest) / 1000 >= 3) return;
        sh_sleep_ms(2000);
    }
}

static int cmp_str(const char *a, const char *b) { return strcmp(a, b); }

static void prune_backups(const char *backup_root, const char *name, size_t keep) {
    char *d = sh_path_join(backup_root, name);
    sh_strv items = SH_STRV_INIT;
    sh_list_dir(d, SH_LIST_DIRS | SH_SORT_NAME, &items);
    for (size_t i = 0; i + keep < items.len; ++i) {
        char *dir = sh_path_join(d, items.items[i]);
        char *sidecar = sh_strfmt("%s.json", dir);  // a backup's description sits next to it (docs/formats.md, section 4)
        sh_remove_tree(dir);
        sh_remove_file(sidecar);
        free(sidecar);
        free(dir);
    }
    sh_strv_free(&items);
    free(d);
}

static bool skip_digest_file(const char *rel) { return strstr(rel, "__pycache__") || sh_starts_with(rel, ".git/"); }

static bool collect_rel(const char *rel, const char *full, bool is_link, void *user) {
    (void)full;
    (void)is_link;
    if (!skip_digest_file(rel)) sh_strv_push(user, rel);
    return true;
}

static char *dir_digest(const char *root) {
    sh_strv files = SH_STRV_INIT;
    sh_walk_files(root, collect_rel, &files);
    sh_strv_sort(&files, cmp_str);
    sh_sha256 h;
    sh_sha256_init(&h);
    for (size_t i = 0; i < files.len; ++i) {
        sh_sha256_update(&h, files.items[i], strlen(files.items[i]));
        char *full = sh_path_join(root, files.items[i]);
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

static const sh_agent *managed_agent(const sh_store *st, const char *id) {
    for (size_t k = 0; k < st->n_agents; ++k)
        if (strcmp(st->agents[k]->id, id) == 0) return st->agents[k];
    return NULL;
}

int sh_store_adopt(sh_store *st) {
    int changes = 0;
    sh_mkpath(st->central);
    sh_strays strays;
    sh_store_strays(st, &strays);
    for (size_t i = 0; i < strays.len; ++i) {
        const sh_stray *s = &strays.items[i];
        const sh_agent *agent = managed_agent(st, s->agent_id);
        if (!agent) continue;
        if (agent->linked && !sh_can_make_dir_links()) {
            sh_store_log(st, "left %s in %s: this account cannot create folder links here (Windows: turn on Developer Mode)",
                         s->name, agent->label);
            continue;
        }
        wait_quiet(s->path);
        char *dest = sh_path_join(st->central, s->name);
        if (!sh_exists(dest)) {
            if (sh_move_dir(s->path, dest)) {
                sh_store_log(st, "adopted %s from %s -> central", s->name, agent->label);
                ++changes;
            } else {
                sh_store_log(st, "FAILED to move %s", s->path);
            }
        } else {
            char *a = dir_digest(s->path), *b = dir_digest(dest), *stamp = sh_stamp();
            const bool same = strcmp(a, b) == 0;
            char *bk = sh_strfmt("%s/%s/%s-%s-%s", st->backup, same ? "_dupes" : "_conflicts", agent->id, s->name, stamp);
            if (sh_move_dir(s->path, bk)) {
                if (same) sh_store_log(st, "removed duplicate %s in %s (backup: %s)", s->name, agent->label, bk);
                else sh_store_log(st, "CONFLICT %s in %s differs from central; kept central, moved yours to %s", s->name, agent->label, bk);
                ++changes;
            }
            free(bk);
            free(stamp);
            free(a);
            free(b);
        }
        free(dest);
    }
    sh_strays_free(&strays);
    changes += sh_store_relink(st);
    return changes;
}

int sh_store_relink(sh_store *st) {
    int changes = 0;
    sh_skills skills;
    sh_store_scan(st, &skills);
    char *central_slash = sh_strfmt("%s/", st->central);
    for (size_t k = 0; k < st->n_agents; ++k) {
        const sh_agent *a = st->agents[k];
        if (!a->linked || !*a->dir) continue;
        sh_mkpath(a->dir);
        for (size_t i = 0; i < skills.len; ++i) {
            const sh_skill *s = &skills.items[i];
            char *lp = sh_path_join(a->dir, s->name);
            const bool is_link = sh_is_link(lp);
            if (is_link || sh_exists(lp)) {
                if (is_link && !sh_exists(lp)) {  // a broken link with a live target name: point it again
                    sh_remove_link(lp);
                } else {
                    free(lp);
                    continue;
                }
            }
            char *rel = sh_path_relative(a->dir, s->path);
            if (sh_make_dir_link(lp, rel)) {
                sh_store_log(st, "linked %s -> %s", lp, rel);
                ++changes;
            } else {
                sh_store_log(st, "FAILED to link %s", lp);
            }
            free(rel);
            free(lp);
        }
        // dead links, but only those Skill Hub could have made
        sh_strv entries = SH_STRV_INIT;
        sh_list_dir(a->dir, SH_LIST_ALL, &entries);
        for (size_t i = 0; i < entries.len; ++i) {
            const char *n = entries.items[i];
            char *p = sh_path_join(a->dir, n);
            if (sh_is_link(p) && !sh_exists(p) && !hidden_name(n) && !sh_strv_contains(&a->ignore, n)) {
                char *target = sh_link_target(p);
                if (target && sh_path_within(target, central_slash) ) {
                    sh_remove_link(p);
                    sh_store_log(st, "removed dead link %s", p);
                    ++changes;
                }
                free(target);
            }
            free(p);
        }
        sh_strv_free(&entries);
    }
    free(central_slash);
    sh_skills_free(&skills);
    bool changed = false;
    sh_store_ensure_provider_configs(st, &changed);
    if (changed) ++changes;
    return changes;
}

// ---- git ----

typedef struct {
    bool ok;
    sh_buf out;
    char *err;  // trimmed stderr, or why git could not run
} git_result;

static void git_result_free(git_result *r) {
    sh_buf_free(&r->out);
    free(r->err);
}

static git_result run_git(const char *const *args, const char *work_dir, int timeout_ms) {
    git_result r = {0};
    const char *argv[64];
    size_t n = 0;
    argv[n++] = sh_git_program();
    argv[n++] = "-c";
    argv[n++] = "core.longpaths=true";  // some skill repositories nest deeper than the 260 characters Windows allows by default
    for (size_t i = 0; args[i] && n < 63; ++i) argv[n++] = args[i];
    argv[n] = NULL;
    static const char *const env[] = {"GIT_TERMINAL_PROMPT=0", NULL};
    const sh_proc_opts o = {work_dir, env, NULL, 0, timeout_ms ? timeout_ms : 180000};
    sh_proc_result p;
    sh_proc_run(argv, &o, &p);
    if (!p.started) {
        r.err = sh_strdup("git was not found: install Git (https://git-scm.com) and make sure it is on your PATH");
    } else if (p.timed_out) {
        r.err = sh_strdup("git timed out");
    } else {
        r.ok = p.exit_code == 0;
        r.err = sh_trimmed(sh_buf_str(&p.err), p.err.len);
    }
    r.out = p.out;
    p.out = (sh_buf)SH_BUF_INIT;
    sh_proc_result_free(&p);
    return r;
}

static char *left_text(const char *s, size_t units) { return sh_left_utf16(s, units); }

static char *repo_dir_for(const sh_store *st, const char *slug) {
    char *key = sh_strdup(slug);
    sh_buf k = SH_BUF_INIT;
    for (const char *c = key; *c; ++c) {
        if (*c == '/') sh_buf_puts(&k, "__");
        else sh_buf_putc(&k, *c);
    }
    free(key);
    char *dir = sh_strfmt("%s/repos/%s", st->cache, sh_buf_str(&k));
    sh_buf_free(&k);
    return dir;
}

bool sh_store_ensure_repo(sh_store *st, const char *slug, const char *url, char **dir_out, char **err) {
    char *dir = repo_dir_for(st, slug);
    if (dir_out) *dir_out = sh_strdup(dir);
    if (sh_strv_contains(&st->fetched, slug)) {
        free(dir);
        return true;
    }
    char *dotgit = sh_path_join(dir, ".git");
    git_result r;
    if (!sh_exists(dotgit)) {
        char *repos = sh_path_join(st->cache, "repos");
        sh_mkpath(repos);
        free(repos);
        sh_remove_tree(dir);
        sh_store_log(st, "cloning %s ...", slug);
        const char *args[] = {"clone", "--depth", "1", "--quiet", "--", url, dir, NULL};
        r = run_git(args, NULL, 0);
    } else {
        const char *fetch[] = {"fetch", "--depth", "1", "--quiet", "origin", "HEAD", NULL};
        r = run_git(fetch, dir, 0);
        if (r.ok) {
            git_result_free(&r);
            const char *checkout[] = {"checkout", "--quiet", "--force", "FETCH_HEAD", NULL};
            r = run_git(checkout, dir, 0);
        }
    }
    free(dotgit);
    const bool ok = r.ok;
    if (!ok) {
        char *short_err = left_text(r.err ? r.err : "", 200);
        if (err) *err = left_text(r.err ? r.err : "", 300);
        sh_store_log(st, "git failed for %s: %s", slug, short_err);
        free(short_err);
    } else {
        sh_strv_push(&st->fetched, slug);
    }
    git_result_free(&r);
    free(dir);
    return ok;
}

char *sh_store_tree_hash(const char *repo, const char *dir) {
    char *spec = *dir ? sh_strfmt("HEAD:%s", dir) : sh_strdup("HEAD^{tree}");
    const char *args[] = {"rev-parse", "--verify", "--quiet", spec, NULL};
    git_result r = run_git(args, repo, 0);
    char *out = r.ok ? sh_trimmed(sh_buf_str(&r.out), r.out.len) : sh_strdup("");
    git_result_free(&r);
    free(spec);
    return out;
}

// rank a SKILL.md location inside a repository; lower is better, NEVER = not a real skill
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
} ranked_path;

static int cmp_ranked(const void *a, const void *b) {
    const ranked_path *x = a, *y = b;
    if (x->rank != y->rank) return x->rank < y->rank ? -1 : 1;
    return strcmp(x->path, y->path);
}

static void skill_paths_in_repo(const char *repo, sh_strv *out) {
    const char *args[] = {"ls-tree", "-r", "-z", "--name-only", "HEAD", NULL};  // -z: paths with any characters come unquoted
    git_result r = run_git(args, repo, 0);
    sh_strv names = SH_STRV_INIT;
    sh_split(sh_buf_str(&r.out), r.out.len, '\0', true, &names);
    ranked_path *ranked = sh_xcalloc(names.len + 1, sizeof *ranked);
    size_t n = 0;
    for (size_t i = 0; i < names.len; ++i) {
        const char *p = names.items[i];
        if (strcmp(p, "SKILL.md") != 0 && !sh_ends_with(p, "/SKILL.md")) continue;
        const int rank = path_rank(p);
        if (rank != NEVER) ranked[n++] = (ranked_path){rank, p};
    }
    qsort(ranked, n, sizeof *ranked, cmp_ranked);
    for (size_t i = 0; i < n; ++i) sh_strv_push(out, ranked[i].path);
    free(ranked);
    sh_strv_free(&names);
    git_result_free(&r);
}

static void show_frontmatter(const char *repo, const char *path, sh_frontmatter *fm) {
    char *spec = sh_strfmt("HEAD:%s", path);
    const char *args[] = {"show", spec, NULL};
    git_result r = run_git(args, repo, 20000);
    if (r.ok) sh_fm_parse(sh_buf_str(&r.out), r.out.len < 64 * 1024 ? r.out.len : 64 * 1024, fm);
    else memset(fm, 0, sizeof *fm);
    git_result_free(&r);
    free(spec);
}

static bool name_matches(const sh_frontmatter *fm, const char *path, const char *name) {
    const char *nm = sh_fm_value(fm, "name");
    if (strcmp(nm, name) == 0) return true;
    if (*nm) return false;
    char *dir = sh_dir_of(path);
    const bool same = strcmp(sh_path_name(dir), name) == 0;
    free(dir);
    return same;
}

// the SKILL.md of the skill called `name`, wherever upstream keeps it
char *sh_store_find_skill_path(const char *repo, const char *name) {
    sh_strv paths = SH_STRV_INIT;
    skill_paths_in_repo(repo, &paths);
    char *found = NULL;
    for (size_t i = 0; !found && i < paths.len; ++i) {
        sh_frontmatter fm;
        show_frontmatter(repo, paths.items[i], &fm);
        if (name_matches(&fm, paths.items[i], name)) found = sh_strdup(paths.items[i]);
        sh_fm_free(&fm);
    }
    sh_strv_free(&paths);
    return found;
}

static bool word_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

static size_t word_run(const char *s) {
    size_t n = 0;
    while (word_char(s[n])) ++n;
    return n;
}

char *sh_slug_from_input(const char *in, char **url) {
    char *s = sh_trimmed(in, strlen(in));
    char *owner = NULL, *repo = NULL;
    // https://github.com/owner/repo[.git][/anything], git@github.com:owner/repo, www.github.com/...
    const char *p = s;
    if (sh_starts_with(p, "https://")) p += 8;
    else if (sh_starts_with(p, "http://")) p += 7;
    else if (sh_starts_with(p, "git@")) p += 4;
    if (sh_starts_with(p, "www.")) p += 4;
    if (sh_starts_with(p, "github.com") && (p[10] == '/' || p[10] == ':')) {
        p += 11;
        const size_t on = word_run(p);
        if (on > 0 && p[on] == '/') {
            const char *r = p + on + 1;
            const size_t max = word_run(r);
            for (size_t len = 1; len <= max && !repo; ++len) {  // the shortest name that the rest of the URL allows
                const char *t = r + len;
                if (!*t || strcmp(t, ".git") == 0 || sh_starts_with(t, ".git/") || *t == '/') {
                    owner = sh_strndup(p, on);
                    repo = sh_strndup(r, len);
                }
            }
        }
    }
    if (!repo) {  // owner/repo
        const size_t on = word_run(s);
        if (on > 0 && s[on] == '/') {
            const size_t rn = word_run(s + on + 1);
            if (rn > 0 && s[on + 1 + rn] == '\0') {
                owner = sh_strndup(s, on);
                repo = sh_strndup(s + on + 1, rn);
            }
        }
    }
    char *slug = NULL;
    if (repo) {
        slug = sh_strfmt("%s/%s", owner, repo);
        if (url) *url = sh_strfmt("https://github.com/%s.git", slug);
    }
    free(owner);
    free(repo);
    free(s);
    return slug;
}

bool sh_store_discover(sh_store *st, const char *repo_input, sh_candidates *out, char **err) {
    memset(out, 0, sizeof *out);
    char *url = NULL;
    char *slug = sh_slug_from_input(repo_input, &url);
    if (!slug) {
        if (err) *err = sh_strdup("expected owner/repo or a github.com URL");
        return false;
    }
    char *repo = NULL;
    if (!sh_store_ensure_repo(st, slug, url, &repo, err)) {
        free(repo);
        free(slug);
        free(url);
        return false;
    }
    sh_strv paths = SH_STRV_INIT, seen = SH_STRV_INIT;
    skill_paths_in_repo(repo, &paths);
    out->items = sh_xcalloc(paths.len + 1, sizeof *out->items);
    int budget = 400;
    for (size_t i = 0; i < paths.len; ++i) {
        if (--budget < 0) break;
        const char *p = paths.items[i];
        sh_frontmatter fm;
        show_frontmatter(repo, p, &fm);
        char *dir = sh_dir_of(p);
        char *name = sh_strdup(*sh_fm_value(&fm, "name") ? sh_fm_value(&fm, "name") : sh_path_name(dir));
        // skill names become folder names: refuse anything path-like
        if (!*name || sh_strv_contains(&seen, name) || strchr(name, '/') || name[0] == '.' || strstr(name, "..")) {
            free(name);
            free(dir);
            sh_fm_free(&fm);
            continue;
        }
        sh_strv_push(&seen, name);
        sh_candidate *c = &out->items[out->len++];
        c->name = name;
        c->description = sh_strdup(sh_fm_value(&fm, "description"));
        c->dir = dir;
        c->slug = sh_strdup(slug);
        c->url = sh_strdup(url);
        sh_fm_free(&fm);
    }
    if (out->len == 0 && err) *err = sh_strfmt("no SKILL.md found in %s", slug);
    sh_strv_free(&paths);
    sh_strv_free(&seen);
    free(repo);
    free(slug);
    free(url);
    return out->len > 0;
}

// Write the plain files of a git tree into `dest`. Links and submodules are skipped, paths are checked, sizes are capped.
static bool path_is_safe(const char *path) {
    if (!*path || path[0] == '/' || strchr(path, '\\')) return false;
    sh_strv seg = SH_STRV_INIT;
    sh_split(path, strlen(path), '/', false, &seg);
    bool sane = true;
    for (size_t i = 0; sane && i < seg.len; ++i) {
        const char *part = seg.items[i];
        if (!*part || strcmp(part, "..") == 0 || strcmp(part, ".") == 0) sane = false;
#ifdef _WIN32
        else if (_stricmp(part, ".git") == 0) sane = false;  // no folder named .git anywhere, in any letter case
#else
        else if (strcasecmp(part, ".git") == 0) sane = false;
#endif
    }
    sh_strv_free(&seg);
    return sane;
}

typedef struct {
    char *path;
    char *sha;
    bool exec;
} tree_entry;

static bool extract_tree(const char *repo, const char *spec, const char *dest, char **err) {
    const char *ls_args[] = {"ls-tree", "-r", "-z", spec, NULL};
    git_result ls = run_git(ls_args, repo, 60000);
    if (!ls.ok) {
        if (err) *err = left_text(ls.err ? ls.err : "", 200);
        git_result_free(&ls);
        return false;
    }
    sh_strv recs = SH_STRV_INIT;
    sh_split(sh_buf_str(&ls.out), ls.out.len, '\0', true, &recs);
    tree_entry *files = sh_xcalloc(recs.len + 1, sizeof *files);
    size_t n = 0;
    for (size_t i = 0; i < recs.len; ++i) {
        const char *rec = recs.items[i];
        const char *tab = strchr(rec, '\t');
        if (!tab) continue;
        sh_strv head = SH_STRV_INIT;
        sh_split(rec, (size_t)(tab - rec), ' ', false, &head);
        const bool blob = head.len == 3 && strcmp(head.items[1], "blob") == 0;     // not submodules (commit) or trees
        const bool plain = blob && (strcmp(head.items[0], "100644") == 0 || strcmp(head.items[0], "100755") == 0);  // 120000 is a link: never recreated
        if (plain && path_is_safe(tab + 1)) {
            files[n].path = sh_strdup(tab + 1);
            files[n].sha = sh_strdup(head.items[2]);
            files[n].exec = strcmp(head.items[0], "100755") == 0;
            ++n;
        }
        sh_strv_free(&head);
    }
    sh_strv_free(&recs);
    git_result_free(&ls);
    bool ok = true;
    if (n > 5000) {
        if (err) *err = sh_strdup("more than 5000 files");
        ok = false;
    }
    if (ok && n > 0) {
        sh_buf input = SH_BUF_INIT;
        for (size_t i = 0; i < n; ++i) sh_buf_printf(&input, "%s\n", files[i].sha);
        const char *argv[] = {sh_git_program(), "cat-file", "--batch", NULL};
        const sh_proc_opts o = {repo, NULL, input.data, input.len, 120000};
        sh_proc_result p;
        const bool ran = sh_proc_run(argv, &o, &p);
        sh_buf_free(&input);
        if (!ran) {
            if (err) *err = sh_strdup(p.started ? "git cat-file failed" : "cannot start git");
            ok = false;
        }
        size_t pos = 0;
        long long total = 0;
        if (ok) sh_mkpath(dest);
        for (size_t i = 0; ok && i < n; ++i) {
            const char *all = sh_buf_str(&p.out);
            const char *nl = memchr(all + pos, '\n', p.out.len - pos);
            long long size = -1;
            if (nl) {
                sh_strv h = SH_STRV_INIT;
                sh_split(all + pos, (size_t)(nl - (all + pos)), ' ', false, &h);
                if (h.len == 3 && strcmp(h.items[1], "blob") == 0) size = atoll(h.items[2]);
                sh_strv_free(&h);
            }
            const size_t body = nl ? (size_t)(nl - all) + 1 : 0;
            if (size < 0 || body + (size_t)size > p.out.len) {
                if (err) *err = sh_strfmt("unexpected git output for %s", files[i].path);
                ok = false;
                break;
            }
            total += size;
            if (total > 128LL * 1024 * 1024) {
                if (err) *err = sh_strdup("more than 128 MB");
                ok = false;
                break;
            }
            char *to = sh_path_join(dest, files[i].path);
            char *parent = sh_path_parent(to);
            sh_mkpath(parent);
            free(parent);
            if (!sh_write_file(to, all + body, (size_t)size)) {
                if (err) *err = sh_strfmt("cannot write %s", files[i].path);
                ok = false;
            } else {
                sh_set_executable(to, files[i].exec);
            }
            free(to);
            pos = body + (size_t)size + 1;
        }
        sh_proc_result_free(&p);
    }
    for (size_t i = 0; i < n; ++i) {
        free(files[i].path);
        free(files[i].sha);
    }
    free(files);
    return ok;
}

// extract repo:dir into a staging folder, check it, and swap it into the shared folder, keeping a backup
static bool stage_and_swap(sh_store *st, const char *name, const char *repo, const char *dir) {
    static unsigned counter;
    char *stage = sh_strfmt("%s/stage/%s-%ld-%u", st->cache, name, (long)(sh_now_ms() % 100000000), ++counter);
    sh_remove_tree(stage);
    sh_mkpath(stage);
    char *spec = *dir ? sh_strfmt("HEAD:%s", dir) : sh_strdup("HEAD");
    char *xerr = NULL;
    bool ok = extract_tree(repo, spec, stage, &xerr);
    free(spec);
    if (!ok) {
        sh_store_log(st, "extract failed for %s: %s", name, xerr ? xerr : "");
        sh_remove_tree(stage);
    }
    free(xerr);
    char *md = ok ? sh_path_join(stage, "SKILL.md") : NULL;
    if (ok) {
        sh_frontmatter fm;
        sh_fm_read(md, &fm);
        if (!sh_exists(md) || !*sh_fm_value(&fm, "description")) {
            sh_store_log(st, "refusing %s: upstream folder has no valid SKILL.md", name);
            sh_remove_tree(stage);
            ok = false;
        }
        sh_fm_free(&fm);
    }
    free(md);
    char *dest = sh_path_join(st->central, name);
    if (ok && sh_exists(dest)) {
        char *stamp = sh_stamp();
        char *bk = sh_strfmt("%s/%s/%s", st->backup, name, stamp);
        if (!sh_move_dir(dest, bk)) {
            sh_store_log(st, "cannot back up %s", dest);
            sh_remove_tree(stage);
            ok = false;
        }
        free(bk);
        free(stamp);
    }
    if (ok) {
        sh_mkpath(st->central);
        if (!sh_move_dir(stage, dest)) {
            sh_store_log(st, "cannot place %s", dest);
            ok = false;
        } else {
            prune_backups(st->backup, name, 3);
        }
    }
    free(dest);
    free(stage);
    return ok;
}

// ---- updates ----

static char *source_url(const sh_source *s) {
    return *s->url ? sh_strdup(s->url) : sh_strfmt("https://github.com/%s.git", s->source);
}

static void set_str(char **field, const char *value) {
    free(*field);
    *field = sh_strdup(value ? value : "");
}

int sh_store_check_updates(sh_store *st, sh_skills *skills, const sh_strv *only) {
    int available = 0;
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
        char *repo = NULL, *err = NULL;
        if (!sh_store_ensure_repo(st, s->src.source, url, &repo, &err)) {
            s->update = SH_UPDATE_ERROR;
            set_str(&s->note, err);
            free(err);
            free(repo);
            free(url);
            continue;
        }
        char *dir = sh_dir_of(s->src.skill_path);
        char *h = *s->src.skill_path ? sh_store_tree_hash(repo, dir) : sh_strdup("");
        bool exists = false;
        if (*h) {
            char *spec = sh_strfmt("HEAD:%s", s->src.skill_path);
            const char *args[] = {"cat-file", "-e", spec, NULL};
            git_result r = run_git(args, repo, 0);
            exists = r.ok;
            git_result_free(&r);
            free(spec);
        }
        bool failed = false;
        if (!exists) {  // upstream moved the folder: look it up by skill name
            char *found = sh_store_find_skill_path(repo, s->name);
            if (!found) {
                s->update = SH_UPDATE_ERROR;
                set_str(&s->note, "not found upstream");
                failed = true;
            } else {
                set_str(&s->new_path, found);
                free(dir);
                dir = sh_dir_of(found);
                free(h);
                h = sh_store_tree_hash(repo, dir);
                free(found);
            }
        }
        if (!failed) {
            set_str(&s->new_hash, h);
            if (strcmp(h, s->src.hash) == 0 && !*s->new_path) {
                s->update = SH_UPDATE_CURRENT;
            } else {
                s->update = SH_UPDATE_AVAILABLE;
                ++available;
            }
        }
        free(h);
        free(dir);
        free(repo);
        free(url);
    }
    return available;
}

bool sh_store_apply_update(sh_store *st, sh_skill *s) {
    if (!sh_source_tracked(&s->src)) return false;
    char *url = source_url(&s->src);
    char *repo = NULL, *err = NULL;
    if (!sh_store_ensure_repo(st, s->src.source, url, &repo, &err)) {
        free(err);
        free(repo);
        free(url);
        return false;
    }
    free(err);
    const char *skill_md = *s->new_path ? s->new_path : s->src.skill_path;
    char *dir = sh_dir_of(skill_md);
    char *hash = *s->new_hash ? sh_strdup(s->new_hash) : sh_store_tree_hash(repo, dir);
    bool ok = *hash && stage_and_swap(st, s->name, repo, dir);
    if (ok) {
        sh_source src;
        sh_source_copy(&s->src, &src);
        set_str(&src.url, url);
        set_str(&src.skill_path, skill_md);
        set_str(&src.hash, hash);
        char *now = sh_now_iso();
        if (!*src.installed_at) set_str(&src.installed_at, now);
        set_str(&src.updated_at, now);
        free(now);
        sh_lock_put(st, s->name, &src);
        sh_source_free(&s->src);
        s->src = src;
        s->update = SH_UPDATE_CURRENT;
        set_str(&s->new_hash, "");
        set_str(&s->new_path, "");
        char *md = sh_strfmt("%s/%s/SKILL.md", st->central, s->name);
        sh_frontmatter fm;
        sh_fm_read(md, &fm);
        set_str(&s->description, sh_fm_value(&fm, "description"));
        sh_fm_free(&fm);
        free(md);
        sh_store_log(st, "updated %s -> %.8s", s->name, hash);
        sh_store_relink(st);
    }
    free(hash);
    free(dir);
    free(repo);
    free(url);
    return ok;
}

bool sh_store_install(sh_store *st, const sh_candidate *c) {
    char *repo = NULL, *err = NULL;
    if (!sh_store_ensure_repo(st, c->slug, c->url, &repo, &err)) {
        free(err);
        free(repo);
        return false;
    }
    free(err);
    char *hash = sh_store_tree_hash(repo, c->dir);
    bool ok = *hash && stage_and_swap(st, c->name, repo, c->dir);
    if (ok) {
        sh_source src = {0};
        src.source = sh_strdup(c->slug);
        src.url = sh_strdup(c->url);
        src.skill_path = *c->dir ? sh_strfmt("%s/SKILL.md", c->dir) : sh_strdup("SKILL.md");
        src.hash = sh_strdup(hash);
        src.installed_at = sh_now_iso();
        src.updated_at = sh_strdup(src.installed_at);
        sh_lock_put(st, c->name, &src);
        sh_source_free(&src);
        sh_store_log(st, "installed %s from %s", c->name, c->slug);
        sh_store_relink(st);
    }
    free(hash);
    free(repo);
    return ok;
}

bool sh_store_remove_skill(sh_store *st, const char *name) {
    if (!*name || strchr(name, '/') || hidden_name(name)) return false;
    char *dest = sh_path_join(st->central, name);
    if (!sh_exists(dest)) {
        free(dest);
        return false;
    }
    char *stamp = sh_stamp();
    char *bk = sh_strfmt("%s/_removed/%s-%s", st->backup, name, stamp);
    const bool moved = sh_move_dir(dest, bk);
    free(bk);
    free(stamp);
    free(dest);
    if (!moved) return false;
    for (size_t k = 0; k < st->n_agents; ++k) {
        char *lp = sh_path_join(st->agents[k]->dir, name);
        sh_remove_link(lp);
        free(lp);
    }
    sh_lock_put(st, name, NULL);
    sh_store_log(st, "removed %s (backup in %s/_removed)", name, st->backup);
    return true;
}

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
    char *repo = NULL;
    if (!sh_store_ensure_repo(st, slug, url, &repo, err)) {
        free(repo);
        free(slug);
        free(url);
        return false;
    }
    char *skill_md = sh_strdup(path ? path : "");
    if (!*skill_md) {
        sh_strv paths = SH_STRV_INIT;
        skill_paths_in_repo(repo, &paths);
        for (size_t i = 0; i < paths.len; ++i) {
            sh_frontmatter fm;
            show_frontmatter(repo, paths.items[i], &fm);
            const bool match = name_matches(&fm, paths.items[i], name);
            sh_fm_free(&fm);
            if (match) {
                set_str(&skill_md, paths.items[i]);
                break;
            }
        }
        sh_strv_free(&paths);
    } else if (!sh_ends_with(skill_md, "SKILL.md")) {
        char *with = sh_strfmt("%s/SKILL.md", skill_md);
        free(skill_md);
        skill_md = with;
    }
    bool ok = *skill_md != '\0';
    if (!ok) {
        if (err) *err = sh_strfmt("skill %s not found in %s", name, slug);
    } else {
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
    free(repo);
    free(slug);
    free(url);
    return ok;
}
