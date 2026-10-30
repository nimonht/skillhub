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
#include "sh_state.h"
#include "sh_thread.h"
#include "sh_time.h"
#include "sh_toggle.h"
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
    st->parked = sh_path_join(st->home, ".agents/.parked");
    st->cache = sh_cache_dir();
    st->state = sh_state_dir();
    st->log_lock = sh_mutex_new();
    sh_git_program();  // found once here, before any worker thread asks for it
    sh_store_reload_providers(st);
}

void sh_store_free(sh_store *st) {
    free(st->home);
    free(st->central);
    free(st->parked);
    free(st->lock_path);
    free(st->lock_other);
    free(st->backup);
    free(st->cache);
    free(st->state);
    for (size_t i = 0; i < st->n_providers; ++i) {
        sh_agent *a = &st->providers[i];
        free(a->id); free(a->label); free(a->mode); free(a->dir); free(a->config_file); free(a->detected_by);
        sh_strv_free(&a->extra_dirs); sh_strv_free(&a->ignore); sh_strv_free(&a->bins); sh_strv_free(&a->detect);
    }
    free(st->providers);
    free(st->agents);
    sh_mutex_free(st->log_lock);
    memset(st, 0, sizeof *st);
}

void sh_store_log(sh_store *st, const char *fmt, ...) {
    sh_buf b = SH_BUF_INIT;
    va_list ap;
    va_start(ap, fmt);
    sh_buf_vprintf(&b, fmt, ap);
    va_end(ap);
    if (st->log) {
        sh_mutex_lock(st->log_lock);
        st->log(st->log_user, sh_buf_str(&b));
        sh_mutex_unlock(st->log_lock);
    }
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
    free(c->name); free(c->description); free(c->dir); free(c->slug); free(c->url); free(c->key);
    memset(c, 0, sizeof *c);
}

void sh_candidates_free(sh_candidates *c) {
    for (size_t i = 0; i < c->len; ++i) sh_candidate_free(&c->items[i]);
    free(c->items);
    c->items = NULL;
    c->len = 0;
}

// ---- scan ----

static sh_link_state state_for(const sh_agent *a, const sh_skill *s, const char *central, const sh_toggles *toggles) {
    const char *name = s->name;
    if (strcmp(a->mode, "link") == 0) {
        char *lp = sh_path_join(a->dir, name);
        const bool off = sh_toggles_off(toggles, name, a->dir);
        sh_link_state st;
        if (sh_is_link(lp)) st = sh_exists(lp) && sh_points_to(lp, s->path) ? (off ? SH_LINK_OFF : SH_LINK_LINKED) : SH_LINK_FOREIGN;
        else if (sh_exists(lp)) st = SH_LINK_COPY;
        else st = off ? SH_LINK_OFF : SH_LINK_MISSING;
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
    return s->parked ? SH_LINK_OFF : SH_LINK_NATIVE;
}

static void fill_links(const sh_store *st, sh_skill *s, const sh_toggles *toggles) {
    s->n_links = st->n_agents;
    s->links = sh_xcalloc(st->n_agents + 1, sizeof *s->links);
    for (size_t k = 0; k < st->n_agents; ++k) s->links[k] = state_for(st->agents[k], s, st->central, toggles);
}

char *sh_store_skill_dir(const sh_store *st, const char *name) {
    char *dir = sh_path_join(st->central, name);
    if (sh_lexists(dir) || !st->parked) return dir;
    char *parked = sh_path_join(st->parked, name);
    if (!sh_lexists(parked)) {
        free(parked);
        return dir;
    }
    free(dir);
    return parked;
}

// the skills in the parked folder, added to `names`; the shared folder wins when both have a name
static void add_parked(const sh_store *st, sh_strv *names) {
    sh_strv parked = SH_STRV_INIT;
    sh_list_dir(st->parked, SH_LIST_ALL, &parked);
    for (size_t i = 0; i < parked.len; ++i)
        if (!sh_strv_contains(names, parked.items[i])) sh_strv_push(names, parked.items[i]);
    if (parked.len) sh_strv_sort(names, sh_compare_ci);
    sh_strv_free(&parked);
}

void sh_store_scan(const sh_store *st, sh_skills *out) {
    memset(out, 0, sizeof *out);
    yyjson_mut_doc *lock = sh_lock_load(st, NULL);  // unreadable: nothing counts as tracked, and nothing is written
    yyjson_mut_val *lock_skills = lock ? yyjson_mut_obj_get(yyjson_mut_doc_get_root(lock), "skills") : NULL;
    sh_json_index *entries = sh_json_index_new(lock_skills);
    sh_strv names = SH_STRV_INIT;
    sh_list_dir(st->central, SH_LIST_DIRS | SH_SORT_NAME_CI, &names);
    const bool any_parked = st->parked && sh_is_dir(st->parked);
    if (any_parked) add_parked(st, &names);
    yyjson_mut_doc *state = any_parked ? sh_state_load(NULL) : NULL;
    yyjson_mut_val *parked_rows = state ? yyjson_mut_obj_get(yyjson_mut_doc_get_root(state), "parked") : NULL;
    sh_json_index *parked_ix = sh_json_index_new(parked_rows);
    sh_toggles *toggles = sh_toggles_load();
    out->items = sh_xcalloc(names.len + 1, sizeof *out->items);
    for (size_t i = 0; i < names.len; ++i) {
        const char *name = names.items[i];
        if (hidden_name(name)) continue;
        char *path = sh_path_join(st->central, name);
        const bool parked = any_parked && !sh_lexists(path);
        if (parked) {
            free(path);
            path = sh_path_join(st->parked, name);
        }
        char *md = sh_path_join(path, "SKILL.md");
        yyjson_mut_val *row = parked ? sh_json_index_get(parked_ix, name) : NULL;
        yyjson_mut_val *entry = parked ? (row ? yyjson_mut_obj_get(row, "lockEntry") : NULL) : sh_json_index_get(entries, name);
        // the only link that is a skill: a live link to a local folder, which the lock file says it is
        const bool live = strcmp(yyjson_mut_is_obj(entry) ? sh_json_mut_str(entry, "sourceType") : "", "local") == 0;
        if (!sh_exists(md) || (sh_is_link(path) && !live)) {
            free(md);
            free(path);
            continue;
        }
        sh_skill *s = &out->items[out->len++];
        memset(s, 0, sizeof *s);
        s->name = sh_strdup(name);
        s->path = path;
        s->parked = parked;
        sh_frontmatter fm;
        sh_fm_read(md, &fm);
        s->description = sh_strdup(sh_fm_value(&fm, "description"));
        sh_fm_free(&fm);
        free(md);
        if (yyjson_mut_is_obj(entry)) sh_source_from_mut_json(entry, &s->src);
        else sh_source_from_json(NULL, &s->src);
        s->live = sh_is_link(path);
        s->update = sh_source_tracked(&s->src) ? SH_UPDATE_UNKNOWN : SH_UPDATE_UNTRACKED;
        s->new_hash = sh_strdup("");
        s->new_path = sh_strdup("");
        s->note = sh_strdup("");
        fill_links(st, s, toggles);
    }
    sh_toggles_free(toggles);
    sh_json_index_free(parked_ix);
    yyjson_mut_doc_free(state);
    sh_strv_free(&names);
    sh_json_index_free(entries);
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
                    char *dest = sh_store_skill_dir(st, n);
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
        char *dest = sh_store_skill_dir(st, s->name);
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
    sh_parked_reconcile(st);
    sh_skills skills;
    sh_store_scan(st, &skills);
    sh_toggles *toggles = sh_toggles_load();
    char *central_slash = sh_strfmt("%s/", st->central), *parked_slash = sh_strfmt("%s/", st->parked);
    for (size_t k = 0; k < st->n_agents; ++k) {
        const sh_agent *a = st->agents[k];
        if (!a->linked || !*a->dir) continue;
        sh_mkpath(a->dir);
        for (size_t i = 0; i < skills.len; ++i) {
            const sh_skill *s = &skills.items[i];
            char *lp = sh_path_join(a->dir, s->name);
            const bool is_link = sh_is_link(lp);
            if (sh_toggles_off(toggles, s->name, a->dir)) {  // only a link of ours goes: never a real folder, or someone else's link
                if (is_link && sh_points_to(lp, s->path) && sh_remove_link(lp)) {
                    sh_store_log(st, "switched %s off for %s", s->name, a->label);
                    ++changes;
                }
                free(lp);
                continue;
            }
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
                if (target && (sh_path_within(target, central_slash) || sh_path_within(target, parked_slash))) {
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
    free(parked_slash);
    free(central_slash);
    sh_toggles_free(toggles);
    sh_skills_free(&skills);
    bool changed = false;
    sh_store_ensure_provider_configs(st, &changed);
    if (changed) ++changes;
    return changes;
}

// ---- sources ----

// ---- staging and swapping ----

char *sh_store_stage_dir(const sh_store *st, const char *label) {
    static long counter;  // shared by every thread; the process id keeps other processes' stages apart
    return sh_strfmt("%s/stage/%s-%ld-%ld", st->cache, label, sh_process_id(), sh_atomic_inc(&counter));
}

// `base` (taken over), plus a counter when that second is taken already. The counter only grows, also past backups pruned
// meanwhile: the name is what says which backup is the newest.
static char *unused_backup(char *base) {
    char *parent = sh_path_parent(base);
    const char *leaf = base + strlen(parent) + 1;
    const size_t n = strlen(leaf);
    sh_strv entries = SH_STRV_INIT;
    sh_list_dir(parent, SH_LIST_ALL, &entries);
    long top = 0;
    for (size_t i = 0; i < entries.len; ++i) {
        const char *e = entries.items[i];
        if (strncmp(e, leaf, n) != 0) continue;
        if (!e[n]) top = top < 1 ? 1 : top;
        else if (e[n] == '-' && e[n + 1] && strspn(e + n + 1, "0123456789") == strlen(e + n + 1) && atol(e + n + 1) > top) top = atol(e + n + 1);
    }
    sh_strv_free(&entries);
    free(parent);
    char *bk = top ? sh_strfmt("%s-%ld", base, top + 1) : sh_strdup(base);
    free(base);
    return bk;
}

static char *backup_dir_for(const sh_store *st, const char *name) {
    char *stamp = sh_stamp();
    char *bk = unused_backup(sh_strfmt("%s/%s/%s", st->backup, name, stamp));
    free(stamp);
    return bk;
}

// The swap journal: one file per swap that replaces a folder, written before the folder is moved aside and removed once
// the new one is in place. A fresh install needs none: its only step is one rename.
static char *journal_write(const sh_store *st, const char *name, const char *dest, const char *backup, const char *stage) {
    static long counter;  // shared by every thread
    char *path = sh_strfmt("%s/journal/%s-%ld-%ld.json", st->state, name, sh_process_id(), sh_atomic_inc(&counter));
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    sh_json_set_int(d, root, "version", 1);
    sh_json_set_str(d, root, "name", name);
    sh_json_set_str(d, root, "dest", dest);
    sh_json_set_str(d, root, "backup", backup);
    sh_json_set_str(d, root, "stage", stage);
    const bool ok = sh_json_write(path, d, true, true);
    yyjson_mut_doc_free(d);
    if (!ok) {
        free(path);
        return NULL;
    }
    return path;
}

static bool has_skill_md(const char *folder) {
    char *md = sh_path_join(folder, "SKILL.md");
    sh_frontmatter fm;
    sh_fm_read(md, &fm);
    const bool ok = sh_exists(md) && *sh_fm_value(&fm, "description");
    sh_fm_free(&fm);
    free(md);
    return ok;
}

// puts `stage` (or, with `link_to`, a link to that folder) where skill `name` belongs, moving any folder there aside
static void sidecar_write(const char *bk, const char *name, const sh_backup_note *note) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    sh_json_set_int(d, root, "version", 1);
    sh_json_set_str(d, root, "name", name);
    sh_json_set_str(d, root, "scope", "user");
    sh_json_set_str(d, root, "reason", note->reason);
    char *now = sh_now_iso();
    sh_json_set_str(d, root, "createdAt", now);
    free(now);
    sh_json_set_str(d, root, "createdBy", "skillhub " SKILLHUB_VERSION);
    sh_json_set_str(d, root, "treeSha", note->tree ? note->tree : "");
    if (note->entry) sh_json_set_val(d, root, "lockEntry", sh_source_to_json(d, note->entry));
    char *path = sh_strfmt("%s.json", bk);
    sh_json_write(path, d, true, true);
    free(path);
    yyjson_mut_doc_free(d);
}

static bool swap_in(sh_store *st, const char *name, const char *stage, const char *link_to, const sh_backup_note *note) {
    bool ok = true;
    char *dest = sh_store_skill_dir(st, name), *bk = NULL, *journal = NULL;
    bool moved_aside = false;
    const bool was_link = sh_is_link(dest);
    if (sh_lexists(dest)) {
        bk = backup_dir_for(st, name);
        // SKILLHUB_STATE set but empty: there is no folder to keep a journal in, so the swap goes ahead without one
        const bool journaled = !*st->state || (journal = journal_write(st, name, dest, bk, stage)) != NULL;
        moved_aside = journaled && sh_move_dir(dest, bk);
        if (!moved_aside) {
            sh_store_log(st, "cannot back up %s", dest);
            ok = false;
        }
    }
    if (ok) {
        sh_mkpath(st->central);
        if (link_to ? !sh_make_dir_link(dest, link_to) : !sh_move_dir(stage, dest)) {
            sh_store_log(st, "cannot place %s", dest);
            ok = false;
        } else {
            if (was_link) sh_remove_link(bk);  // a live link: the folder it led to is still there, nothing to keep
            else if (moved_aside && note) sidecar_write(bk, name, note);
            prune_backups(st->backup, name, 3);
        }
    }
    if (!ok && moved_aside && !sh_lexists(dest)) sh_move_dir(bk, dest);  // the old version goes straight back
    if (journal && sh_lexists(dest)) sh_remove_file(journal);  // otherwise recovery puts the backup back later
    free(journal);
    free(bk);
    free(dest);
    return ok;
}

bool sh_store_place(sh_store *st, const char *name, const char *stage, const sh_backup_note *note) {
    bool ok = has_skill_md(stage);
    if (!ok) sh_store_log(st, "refusing %s: upstream folder has no valid SKILL.md", name);
    else ok = swap_in(st, name, stage, NULL, note);
    if (!ok) sh_remove_tree(stage);
    return ok;
}

bool sh_store_place_link(sh_store *st, const char *name, const char *target, const sh_backup_note *note) {
    if (!has_skill_md(target)) {
        sh_store_log(st, "refusing %s: upstream folder has no valid SKILL.md", name);
        return false;
    }
    return swap_in(st, name, "", target, note);
}

bool sh_store_put_back(sh_store *st, const char *name, const char *backup, const sh_backup_note *note) {
    return has_skill_md(backup) && swap_in(st, name, backup, NULL, note);
}

// a path from a journal that may be touched: inside the folder it belongs to
static bool inside(const char *path, const char *dir) {
    char *with_slash = sh_strfmt("%s/", dir);
    const bool yes = *path && sh_path_within(path, with_slash) && !strstr(path, "/../");
    free(with_slash);
    return yes;
}

static int recover_swaps(sh_store *st) {
    if (!*st->state) return 0;  // no state folder, so no journal either
    char *dir = sh_path_join(st->state, "journal");
    sh_strv files = SH_STRV_INIT;
    sh_list_dir(dir, SH_LIST_ALL | SH_SORT_NAME, &files);
    char *stage_root = sh_path_join(st->cache, "stage");
    int put_back = 0;
    for (size_t i = 0; i < files.len; ++i) {
        char *path = sh_path_join(dir, files.items[i]);
        yyjson_doc *doc = NULL;
        if (sh_ends_with(files.items[i], ".json") && sh_json_read(path, 64 * 1024, &doc) == SH_JSON_OK) {
            yyjson_val *root = yyjson_doc_get_root(doc);
            const char *dest = sh_json_str(root, "dest"), *backup = sh_json_str(root, "backup"), *stage = sh_json_str(root, "stage");
            if ((inside(dest, st->central) || inside(dest, st->parked)) && inside(backup, st->backup) && !sh_lexists(dest) && sh_is_dir(backup) &&
                sh_move_dir(backup, dest)) {
                sh_store_log(st, "recovered %s: put back the version an interrupted update had moved aside", sh_json_str(root, "name"));
                ++put_back;
            }
            if (inside(stage, stage_root)) sh_remove_tree(stage);
        }
        yyjson_doc_free(doc);
        sh_remove_file(path);
        free(path);
    }
    sh_strv_free(&files);
    free(stage_root);
    free(dir);
    return put_back;
}

#define STALE_MS (10 * 60 * 1000)  // older leftovers are stale; younger ones may belong to a running 1.x, which takes no lock

#define REVIEW_MS (24 * 60 * 60 * 1000)  // a reviewed update waits for the user to finish reading

static bool stale(const char *path) { return sh_now_ms() - sh_mtime_ms(path) > STALE_MS; }

static void remove_git_locks_of(const char *clone) {
    char *lock_path = sh_strfmt("%s.lock", clone);
    sh_flock *idle = sh_flock_take(lock_path, 0);
    if (idle) {
        char *dotgit = sh_path_join(clone, ".git");
        sh_strv entries = SH_STRV_INIT;
        sh_list_dir(dotgit, SH_LIST_ALL, &entries);
        for (size_t k = 0; k < entries.len; ++k) {
            if (!sh_ends_with(entries.items[k], ".lock")) continue;
            char *f = sh_path_join(dotgit, entries.items[k]);
            if (stale(f)) sh_remove_file(f);
            free(f);
        }
        sh_strv_free(&entries);
        free(dotgit);
        sh_flock_release(idle);
    }
    free(lock_path);
}

// git lock files that a killed git left in a cache clone nobody uses now; clones sit directly in repos/ (GitHub) or in a
// folder per host
static void remove_git_locks(const sh_store *st) {
    char *repos = sh_path_join(st->cache, "repos");
    sh_strv names = SH_STRV_INIT;
    sh_list_dir(repos, SH_LIST_DIRS | SH_LIST_NO_SYMLINKS, &names);
    for (size_t i = 0; i < names.len; ++i) {
        char *path = sh_path_join(repos, names.items[i]);
        char *dotgit = sh_path_join(path, ".git");
        if (sh_exists(dotgit)) {
            remove_git_locks_of(path);
        } else {
            sh_strv clones = SH_STRV_INIT;
            sh_list_dir(path, SH_LIST_DIRS | SH_LIST_NO_SYMLINKS, &clones);
            for (size_t k = 0; k < clones.len; ++k) {
                char *clone = sh_path_join(path, clones.items[k]);
                remove_git_locks_of(clone);
                free(clone);
            }
            sh_strv_free(&clones);
        }
        free(dotgit);
        free(path);
    }
    sh_strv_free(&names);
    free(repos);
}

int sh_store_recover(sh_store *st, bool git_locks) {
    const int put_back = recover_swaps(st);
    char *stage_root = sh_path_join(st->cache, "stage");  // the mutation lock is ours: an old stage belongs to nobody
    sh_strv stages = SH_STRV_INIT;
    sh_list_dir(stage_root, SH_LIST_ALL, &stages);
    for (size_t i = 0; i < stages.len; ++i) {
        char *p = sh_path_join(stage_root, stages.items[i]);
        const bool review = sh_starts_with(stages.items[i], "review-");
        if (review ? sh_now_ms() - sh_mtime_ms(p) > REVIEW_MS : stale(p)) sh_remove_tree(p);
        free(p);
    }
    sh_strv_free(&stages);
    free(stage_root);
    if (git_locks) remove_git_locks(st);
    return put_back;
}

bool sh_store_remove_skill(sh_store *st, const char *name) {
    if (!*name || strchr(name, '/') || hidden_name(name)) return false;
    char *dest = sh_store_skill_dir(st, name);
    if (!sh_lexists(dest)) {  // lexists: a live link whose folder is gone can still be removed
        free(dest);
        return false;
    }
    const bool live = sh_is_link(dest);  // only the link goes; the folder is the author's, and needs no backup
    char *stamp = sh_stamp();
    char *bk = unused_backup(sh_strfmt("%s/_removed/%s-%s", st->backup, name, stamp));
    sh_source entry;
    const bool tracked = sh_lock_entry(st, name, &entry);
    sh_trees *t = tracked ? sh_trees_load(st) : NULL;
    char *tree = sh_strdup(sh_trees_get(t, name));
    sh_trees_free(t);
    const bool gone = live ? sh_remove_link(dest) : sh_move_dir(dest, bk);
    if (gone && !live) {
        const sh_backup_note note = {"remove", tracked ? &entry : NULL, tree};
        sidecar_write(bk, name, &note);
    }
    free(tree);
    if (tracked) sh_source_free(&entry);
    free(bk);
    free(stamp);
    free(dest);
    if (!gone) return false;
    for (size_t k = 0; k < st->n_agents; ++k) {
        char *lp = sh_path_join(st->agents[k]->dir, name);
        sh_remove_link(lp);
        free(lp);
    }
    sh_lock_put(st, name, NULL);
    sh_toggles_forget(name);
    if (live) sh_store_log(st, "removed the link %s", name);
    else sh_store_log(st, "removed %s (backup in %s/_removed)", name, st->backup);
    return true;
}
