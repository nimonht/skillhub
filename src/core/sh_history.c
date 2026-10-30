#include "sh_history.h"

#include <stdlib.h>
#include <string.h>

#include "sh_collate.h"
#include "sh_fs.h"
#include "sh_json.h"
#include "sh_link.h"
#include "sh_path.h"
#include "sh_state.h"
#include "sh_time.h"

// ---- backups ----

// yyyyMMdd-HHmmss, with -2, -3, ... when that second was taken
static bool is_stamp(const char *s) {
    for (size_t i = 0; i < 15; ++i) {
        if (i == 8 ? s[i] != '-' : s[i] < '0' || s[i] > '9') return false;
    }
    if (!s[15]) return true;
    if (s[15] != '-' || !s[16]) return false;
    for (const char *c = s + 16; *c; ++c)
        if (*c < '0' || *c > '9') return false;
    return true;
}

static long counter_of(const char *stamp) { return strlen(stamp) > 16 ? atol(stamp + 16) : 1; }

static int newest_first(const void *x, const void *y) {
    const sh_version *a = x, *b = y;
    const int c = strncmp(b->stamp, a->stamp, 15);
    if (c) return c;
    const long ca = counter_of(a->stamp), cb = counter_of(b->stamp);
    return ca == cb ? 0 : ca < cb ? 1 : -1;
}

static yyjson_doc *read_sidecar(const char *backup) {
    char *path = sh_strfmt("%s.json", backup);
    yyjson_doc *doc = NULL;
    if (sh_json_read(path, 1024 * 1024, &doc) != SH_JSON_OK) doc = NULL;
    free(path);
    return doc;
}

void sh_history_list(const sh_store *st, const char *name, sh_versions *out) {
    memset(out, 0, sizeof *out);
    char *dir = sh_path_join(st->backup, name);
    sh_strv stamps = SH_STRV_INIT;
    sh_list_dir(dir, SH_LIST_DIRS, &stamps);
    out->items = sh_xcalloc(stamps.len + 1, sizeof *out->items);
    for (size_t i = 0; i < stamps.len; ++i) {
        if (!is_stamp(stamps.items[i])) continue;
        char *bk = sh_path_join(dir, stamps.items[i]);
        yyjson_doc *doc = read_sidecar(bk);
        yyjson_val *root = yyjson_doc_get_root(doc);
        sh_version *v = &out->items[out->len++];
        v->stamp = sh_strdup(stamps.items[i]);
        v->reason = sh_strdup(root ? sh_json_str(root, "reason") : "");
        v->created_at = sh_strdup(root ? sh_json_str(root, "createdAt") : "");
        v->tree = sh_strdup(root ? sh_json_str(root, "treeSha") : "");
        v->has_entry = root && yyjson_is_obj(yyjson_obj_get(root, "lockEntry"));
        yyjson_doc_free(doc);
        free(bk);
    }
    qsort(out->items, out->len, sizeof *out->items, newest_first);
    sh_strv_free(&stamps);
    free(dir);
}

void sh_versions_free(sh_versions *v) {
    for (size_t i = 0; i < v->len; ++i) {
        free(v->items[i].stamp);
        free(v->items[i].reason);
        free(v->items[i].created_at);
        free(v->items[i].tree);
    }
    free(v->items);
    memset(v, 0, sizeof *v);
}

// a name that is one folder in the shared folder, wherever it came from
static bool plain_name(const char *n) { return *n && n[0] != '.' && !strchr(n, '/') && !strchr(n, '\\'); }

// the upstream tree id the installed skill came from: the lock has it for GitHub and local folders, the record for the rest
static const char *tree_of(const sh_source *s, const sh_trees *t, const char *name) {
    return strcmp(s->type, "gitlab") == 0 || strcmp(s->type, "git") == 0 ? sh_trees_get(t, name) : s->hash;
}

// after a version came back: the lock entry it had, or (no sidecar) the current one with its hash cleared
static void restore_entry(sh_store *st, const char *name, yyjson_doc *sidecar, sh_source *current) {
    yyjson_val *root = yyjson_doc_get_root(sidecar);
    yyjson_val *entry = root ? yyjson_obj_get(root, "lockEntry") : NULL;
    char *folder = sh_store_skill_dir(st, name);
    char *content = sh_folder_hash(folder);
    free(folder);
    const char *const names[] = {name};
    const char *const contents[] = {content};
    if (yyjson_is_obj(entry)) {
        sh_source kept = {0};
        sh_source_from_json(entry, &kept);
        sh_lock_put(st, name, &kept);
        sh_source_free(&kept);
        const char *const trees[] = {sh_json_str(root, "treeSha")};
        sh_trees_put_many(st, names, trees, contents, 1);
    } else if (current) {
        free(current->hash);
        current->hash = sh_strdup("");
        sh_lock_put(st, name, current);
        const char *const trees[] = {""};
        sh_trees_put_many(st, names, trees, contents, 1);
    }
    free(content);
}

bool sh_history_rollback(sh_store *st, const char *name, const char *stamp, char **err) {
    char *dest = plain_name(name) ? sh_store_skill_dir(st, name) : NULL;
    if (!dest || !sh_is_dir(dest)) {
        if (err) *err = sh_strfmt("%s is not installed", name);
        free(dest);
        return false;
    }
    if (sh_is_link(dest)) {
        if (err) *err = sh_strfmt("%s is a live link: its folder keeps no versions here", name);
        free(dest);
        return false;
    }
    free(dest);
    sh_versions v;
    sh_history_list(st, name, &v);
    const sh_version *pick = NULL;
    for (size_t i = 0; i < v.len && !pick; ++i)
        if (!stamp || !*stamp || strcmp(v.items[i].stamp, stamp) == 0) pick = &v.items[i];
    if (!pick) {
        if (err) *err = stamp && *stamp ? sh_strfmt("%s has no version %s", name, stamp) : sh_strfmt("%s has no earlier version", name);
        sh_versions_free(&v);
        return false;
    }
    char *bk = sh_strfmt("%s/%s/%s", st->backup, name, pick->stamp);
    yyjson_doc *sidecar = read_sidecar(bk);
    sh_source current;
    const bool tracked = sh_lock_entry(st, name, &current);
    sh_trees *t = sh_trees_load(st);
    char *from = sh_strdup(tracked ? tree_of(&current, t, name) : "");
    sh_trees_free(t);
    const sh_backup_note note = {"rollback", tracked ? &current : NULL, from};
    const bool ok = sh_store_put_back(st, name, bk, &note);
    if (ok) {
        char *side = sh_strfmt("%s.json", bk);
        sh_remove_file(side);
        free(side);
        restore_entry(st, name, sidecar, tracked ? &current : NULL);
        if (tracked) sh_hold_set(st, name, from, NULL);
        sh_store_relink(st);
        sh_store_log(st, "rolled back %s to %s", name, pick->stamp);
    } else if (err) {
        *err = sh_strfmt("cannot put back %s", pick->stamp);
    }
    free(from);
    if (tracked) sh_source_free(&current);
    yyjson_doc_free(sidecar);
    free(bk);
    sh_versions_free(&v);
    return ok;
}

bool sh_history_restore(sh_store *st, const char *name, char **err) {
    char *dest = plain_name(name) ? sh_store_skill_dir(st, name) : NULL;
    if (!dest || sh_lexists(dest)) {
        if (err) *err = dest ? sh_strfmt("%s is installed", name) : sh_strfmt("%s is not a skill name", name);
        free(dest);
        return false;
    }
    free(dest);
    char *removed = sh_path_join(st->backup, "_removed");
    sh_strv all = SH_STRV_INIT;
    sh_list_dir(removed, SH_LIST_DIRS, &all);
    const size_t n = strlen(name);
    sh_version newest = {0};
    for (size_t i = 0; i < all.len; ++i) {
        const char *e = all.items[i];
        if (strncmp(e, name, n) != 0 || e[n] != '-' || !is_stamp(e + n + 1)) continue;
        const sh_version candidate = {(char *)e + n + 1, NULL, NULL, NULL, false};
        if (!newest.stamp || newest_first(&candidate, &newest) < 0) newest = candidate;
    }
    bool ok = false;
    if (!newest.stamp) {
        if (err) *err = sh_strfmt("no removed copy of %s", name);
    } else {
        char *bk = sh_strfmt("%s/%s-%s", removed, name, newest.stamp);
        yyjson_doc *sidecar = read_sidecar(bk);
        ok = sh_store_put_back(st, name, bk, NULL);
        if (ok) {
            char *side = sh_strfmt("%s.json", bk);
            sh_remove_file(side);
            free(side);
            restore_entry(st, name, sidecar, NULL);
            sh_store_relink(st);
            sh_store_log(st, "restored %s", name);
        } else if (err) {
            *err = sh_strfmt("cannot put back %s", name);
        }
        yyjson_doc_free(sidecar);
        free(bk);
    }
    sh_strv_free(&all);
    free(removed);
    return ok;
}

// ---- holds, in state.json ----

struct sh_holds {
    yyjson_mut_doc *doc;
    sh_json_index *ix;
};

static yyjson_mut_val *user_holds(yyjson_mut_doc *m) {
    return sh_json_get_or_add_obj(m, sh_json_get_or_add_obj(m, yyjson_mut_doc_get_root(m), "holds"), "user");
}

sh_holds *sh_holds_load(const sh_store *st) {
    (void)st;
    sh_holds *h = sh_xcalloc(1, sizeof *h);
    h->doc = sh_state_load(NULL);
    h->ix = sh_json_index_new(h->doc ? user_holds(h->doc) : NULL);
    return h;
}

const char *sh_holds_get(const sh_holds *h, const char *name) {
    yyjson_mut_val *entry = h ? sh_json_index_get(h->ix, name) : NULL;
    return entry ? sh_json_mut_str(entry, "fromTree") : NULL;
}

void sh_holds_free(sh_holds *h) {
    if (!h) return;
    sh_json_index_free(h->ix);
    yyjson_mut_doc_free(h->doc);
    free(h);
}

bool sh_hold_set(sh_store *st, const char *name, const char *from_tree, char **err) {
    (void)st;
    yyjson_mut_doc *m = sh_state_load(err);
    if (!m) return false;
    yyjson_mut_val *holds = user_holds(m);
    yyjson_mut_val *entry = yyjson_mut_obj(m);
    sh_json_set_str(m, entry, "fromTree", from_tree);
    char *now = sh_now_iso();
    sh_json_set_str(m, entry, "since", now);
    free(now);
    sh_json_set_val(m, holds, name, entry);
    const bool ok = sh_state_save(m);
    if (!ok && err) *err = sh_strdup("cannot write state.json");
    yyjson_mut_doc_free(m);
    return ok;
}

bool sh_holds_release(sh_store *st, const char *const *names, size_t n) {
    (void)st;
    yyjson_mut_doc *m = sh_state_load(NULL);
    if (!m) return false;
    yyjson_mut_val *holds = user_holds(m);
    bool changed = false;
    for (size_t i = 0; i < n; ++i) {
        if (!yyjson_mut_obj_get(holds, names[i])) continue;
        yyjson_mut_obj_remove_str(holds, names[i]);
        changed = true;
    }
    const bool ok = !changed || sh_state_save(m);
    yyjson_mut_doc_free(m);
    return ok && changed;
}
