#include "sh_toggle.h"

#include <stdlib.h>
#include <string.h>

#include "sh_fs.h"
#include "sh_json.h"
#include "sh_link.h"
#include "sh_path.h"
#include "sh_state.h"
#include "sh_time.h"

// ---- toggles.json ----

struct sh_toggles {
    yyjson_mut_doc *doc;
    sh_json_index *ix;
};

static char *toggles_path(void) {
    char *config = sh_config_dir();
    char *p = sh_path_join(config, "toggles.json");
    free(config);
    return p;
}

// the file as a document with `disabled` present; NULL when it exists and cannot be read: it is then never overwritten
static yyjson_mut_doc *toggles_read(char **err) {
    char *path = toggles_path();
    yyjson_doc *doc = NULL;
    const sh_json_status s = sh_json_read(path, 0, &doc);
    free(path);
    if (s != SH_JSON_OK && s != SH_JSON_MISSING) {
        if (err) *err = sh_strdup("toggles.json cannot be read");
        return NULL;
    }
    yyjson_mut_doc *m = doc ? yyjson_doc_mut_copy(doc, NULL) : yyjson_mut_doc_new(NULL);
    yyjson_doc_free(doc);
    if (!yyjson_mut_is_obj(yyjson_mut_doc_get_root(m))) yyjson_mut_doc_set_root(m, yyjson_mut_obj(m));
    yyjson_mut_val *root = yyjson_mut_doc_get_root(m);
    if (!yyjson_mut_obj_get(root, "version")) sh_json_set_int(m, root, "version", 1);
    sh_json_get_or_add_obj(m, root, "disabled");
    return m;
}

static yyjson_mut_val *disabled_of(yyjson_mut_doc *m) { return yyjson_mut_obj_get(yyjson_mut_doc_get_root(m), "disabled"); }

static bool toggles_write(yyjson_mut_doc *m) {
    char *path = toggles_path();
    const bool ok = sh_json_write(path, m, true, true);
    free(path);
    return ok;
}

sh_toggles *sh_toggles_load(void) {
    sh_toggles *t = sh_xcalloc(1, sizeof *t);
    t->doc = toggles_read(NULL);
    t->ix = sh_json_index_new(t->doc ? disabled_of(t->doc) : NULL);
    return t;
}

static bool list_has(yyjson_mut_val *list, const char *dir) {
    size_t i, n;
    yyjson_mut_val *v;
    yyjson_mut_arr_foreach(list, i, n, v)
        if (yyjson_mut_is_str(v) && strcmp(yyjson_mut_get_str(v), dir) == 0) return true;
    return false;
}

bool sh_toggles_off(const sh_toggles *t, const char *name, const char *agent_dir) {
    yyjson_mut_val *list = t ? sh_json_index_get(t->ix, name) : NULL;
    return list && list_has(list, agent_dir);
}

void sh_toggles_free(sh_toggles *t) {
    if (!t) return;
    sh_json_index_free(t->ix);
    yyjson_mut_doc_free(t->doc);
    free(t);
}

void sh_toggles_forget(const char *name) {
    yyjson_mut_doc *m = toggles_read(NULL);
    if (m && yyjson_mut_obj_get(disabled_of(m), name)) {
        yyjson_mut_obj_remove_str(disabled_of(m), name);
        toggles_write(m);
    }
    yyjson_mut_doc_free(m);
}

static void toggle_one(yyjson_mut_doc *m, const char *name, const char *dir, bool on) {
    yyjson_mut_val *disabled = disabled_of(m);
    yyjson_mut_val *list = yyjson_mut_obj_get(disabled, name);
    if (on) {
        if (!list) return;
        size_t i, n;
        yyjson_mut_val *v;
        yyjson_mut_arr_foreach(list, i, n, v) {
            if (!yyjson_mut_is_str(v) || strcmp(yyjson_mut_get_str(v), dir) != 0) continue;
            yyjson_mut_arr_remove(list, i);
            break;
        }
        if (yyjson_mut_arr_size(list) == 0) yyjson_mut_obj_remove_str(disabled, name);
        return;
    }
    if (!yyjson_mut_is_arr(list)) {
        list = yyjson_mut_arr(m);
        sh_json_set_val(m, disabled, name, list);
    }
    if (!list_has(list, dir)) yyjson_mut_arr_add_strcpy(m, list, dir);
}

// ---- parked skills ----

static yyjson_mut_val *parked_of(yyjson_mut_doc *m) { return sh_json_get_or_add_obj(m, yyjson_mut_doc_get_root(m), "parked"); }

bool sh_store_is_parked(const sh_store *st, const char *name) {
    char *central = sh_path_join(st->central, name), *parked = sh_path_join(st->parked, name);
    const bool yes = !sh_lexists(central) && sh_lexists(parked);
    free(parked);
    free(central);
    return yes;
}

bool sh_parked_entry(const sh_store *st, const char *name, sh_source *out) {
    (void)st;
    yyjson_mut_doc *m = sh_state_load(NULL);
    yyjson_mut_val *row = m ? yyjson_mut_obj_get(parked_of(m), name) : NULL;
    yyjson_mut_val *entry = row ? yyjson_mut_obj_get(row, "lockEntry") : NULL;
    const bool found = yyjson_mut_is_obj(entry);
    if (found) sh_source_from_mut_json(entry, out);
    yyjson_mut_doc_free(m);
    return found;
}

static void set_row(yyjson_mut_doc *m, yyjson_mut_val *parked, const char *name, const sh_source *src) {
    yyjson_mut_val *row = yyjson_mut_obj(m);
    if (src) sh_json_set_val(m, row, "lockEntry", sh_source_to_json(m, src));
    char *now = sh_now_iso();
    sh_json_set_str(m, row, "parkedAt", now);
    free(now);
    sh_json_set_val(m, parked, name, row);
}

void sh_parked_put(sh_store *st, const char *name, const sh_source *src) {
    yyjson_mut_doc *m = sh_state_load(NULL);
    if (!m) {
        sh_store_log(st, "FAILED to record %s: state.json cannot be read", name);
        return;
    }
    set_row(m, parked_of(m), name, src);
    if (!sh_state_save(m)) sh_store_log(st, "FAILED to record %s", name);
    yyjson_mut_doc_free(m);
}

void sh_parked_forget(sh_store *st, const char *name) {
    yyjson_mut_doc *m = sh_state_load(NULL);
    if (m && yyjson_mut_obj_get(parked_of(m), name)) {
        yyjson_mut_obj_remove_str(parked_of(m), name);
        if (!sh_state_save(m)) sh_store_log(st, "FAILED to record the removal of %s", name);
    }
    yyjson_mut_doc_free(m);
}

// The lock entries go to state.json before any folder moves: a crash then leaves an entry in both places, which
// sh_parked_reconcile clears up, and never a skill that lost its source.
static bool park_many(sh_store *st, const char *const *names, size_t n, char **err) {
    yyjson_mut_doc *state = sh_state_load(err);
    if (!state) return false;
    char *lock_err = NULL;
    yyjson_mut_doc *lock = sh_lock_load(st, &lock_err);
    if (!lock) {
        if (err) *err = lock_err;
        else free(lock_err);
        yyjson_mut_doc_free(state);
        return false;
    }
    yyjson_mut_val *lock_skills = yyjson_mut_obj_get(yyjson_mut_doc_get_root(lock), "skills");
    sh_json_index *ix = sh_json_index_new(lock_skills);
    yyjson_mut_val *parked = parked_of(state);
    const char **to_move = sh_xcalloc(n + 1, sizeof *to_move);
    size_t k = 0;
    for (size_t i = 0; i < n; ++i) {
        char *central = sh_path_join(st->central, names[i]);
        const bool here = sh_lexists(central);
        free(central);
        if (!here) continue;  // parked already, or not there at all
        yyjson_mut_val *row = yyjson_mut_obj(state);
        yyjson_mut_val *entry = sh_json_index_get(ix, names[i]);
        if (yyjson_mut_is_obj(entry)) sh_json_set_val(state, row, "lockEntry", yyjson_mut_val_mut_copy(state, entry));
        char *now = sh_now_iso();
        sh_json_set_str(state, row, "parkedAt", now);
        free(now);
        sh_json_set_val(state, parked, names[i], row);
        to_move[k++] = names[i];
    }
    sh_json_index_free(ix);
    yyjson_mut_doc_free(lock);
    bool ok = k == 0 || sh_state_save(state);
    if (!ok && err) *err = sh_strdup("cannot write state.json");
    const char **moved = sh_xcalloc(k + 1, sizeof *moved);
    const sh_source **none = sh_xcalloc(k + 1, sizeof *none);
    size_t m = 0;
    sh_mkpath(st->parked);
    for (size_t i = 0; ok && i < k; ++i) {
        char *from = sh_path_join(st->central, to_move[i]), *to = sh_path_join(st->parked, to_move[i]);
        if (sh_move_dir(from, to)) moved[m++] = to_move[i];
        else sh_store_log(st, "FAILED to park %s", to_move[i]);
        free(from);
        free(to);
    }
    if (m) ok = sh_lock_files_put(st, moved, none, m) && ok;
    free(none);
    free(moved);
    free(to_move);
    yyjson_mut_doc_free(state);
    return ok;
}

static void set_aside(sh_store *st, const char *name) {
    char *from = sh_path_join(st->parked, name);
    char *stamp = sh_stamp();
    char *to = sh_strfmt("%s/.parked-replaced/%s-%s", st->backup, name, stamp);
    if (sh_move_dir(from, to)) sh_store_log(st, "%s came back to the shared folder; kept the parked copy in %s", name, to);
    free(to);
    free(stamp);
    free(from);
}

// back to the shared folder; the state entry goes last, so a crash loses nothing
static bool unpark_many(sh_store *st, const char *const *names, size_t n, char **err) {
    yyjson_mut_doc *state = sh_state_load(err);
    if (!state) return false;
    yyjson_mut_val *parked = parked_of(state);
    const char **back = sh_xcalloc(n + 1, sizeof *back);
    sh_source *entries = sh_xcalloc(n + 1, sizeof *entries);
    const sh_source **srcs = sh_xcalloc(n + 1, sizeof *srcs);
    size_t k = 0;
    bool changed = false;
    sh_mkpath(st->central);
    for (size_t i = 0; i < n; ++i) {
        char *from = sh_path_join(st->parked, names[i]), *to = sh_path_join(st->central, names[i]);
        if (sh_lexists(from)) {
            if (sh_lexists(to)) {
                set_aside(st, names[i]);
            } else if (sh_move_dir(from, to)) {
                yyjson_mut_val *row = yyjson_mut_obj_get(parked, names[i]);
                yyjson_mut_val *entry = row ? yyjson_mut_obj_get(row, "lockEntry") : NULL;
                if (yyjson_mut_is_obj(entry)) {
                    sh_source_from_mut_json(entry, &entries[k]);
                    srcs[k] = &entries[k];
                    back[k++] = names[i];
                }
            } else {
                sh_store_log(st, "FAILED to bring back %s", names[i]);
                free(from);
                free(to);
                continue;
            }
            yyjson_mut_obj_remove_str(parked, names[i]);
            changed = true;
        }
        free(from);
        free(to);
    }
    bool ok = k == 0 || sh_lock_files_put(st, back, srcs, k);
    if (changed && ok) ok = sh_state_save(state);
    if (!ok && err && !*err) *err = sh_strdup("cannot record the change");
    for (size_t i = 0; i < k; ++i) sh_source_free(&entries[i]);
    free(srcs);
    free(entries);
    free(back);
    yyjson_mut_doc_free(state);
    return ok;
}

void sh_parked_reconcile(sh_store *st) {
    if (!st->parked || !sh_is_dir(st->parked)) return;
    sh_strv names = SH_STRV_INIT;
    sh_list_dir(st->parked, SH_LIST_ALL, &names);
    for (size_t i = 0; i < names.len; ++i) {
        char *central = sh_path_join(st->central, names.items[i]);
        const bool both = sh_lexists(central);
        free(central);
        if (!both) continue;
        set_aside(st, names.items[i]);
        sh_parked_forget(st, names.items[i]);
    }
    sh_strv_free(&names);
    // a crash between the moves: the entry was written to state.json and the folder stayed
    yyjson_mut_doc *state = sh_state_load(NULL);
    if (!state) return;
    sh_strv stale = SH_STRV_INIT;
    size_t i, n;
    yyjson_mut_val *key, *val;
    yyjson_mut_obj_foreach(parked_of(state), i, n, key, val) {
        char *folder = sh_path_join(st->parked, yyjson_mut_get_str(key));
        if (!sh_lexists(folder)) sh_strv_push(&stale, yyjson_mut_get_str(key));
        free(folder);
    }
    for (size_t k = 0; k < stale.len; ++k) yyjson_mut_obj_remove_str(parked_of(state), stale.items[k]);
    if (stale.len) sh_state_save(state);
    sh_strv_free(&stale);
    yyjson_mut_doc_free(state);
}

int sh_unpark_all(sh_store *st) {
    sh_strv names = SH_STRV_INIT;
    sh_list_dir(st->parked, SH_LIST_ALL, &names);
    const bool ok = unpark_many(st, (const char *const *)names.items, names.len, NULL);
    const int n = ok ? (int)names.len : 0;
    sh_strv_free(&names);
    sh_store_relink(st);
    return n;
}

// ---- switching ----

bool sh_skills_set_enabled(sh_store *st, const char *const *names, size_t n, const char *agent, bool on, char **err) {
    if (st->project) {
        if (err) *err = sh_strdup("a project's skills are shared with its team: they have no personal switches");
        return false;
    }
    const bool all = strcmp(agent, SH_AGENTS_ALL) == 0;
    bool shared = all || strcmp(agent, SH_AGENTS_SHARED) == 0;
    const sh_agent *one = NULL;
    if (!shared) {
        one = sh_store_find_agent(st, agent);
        if (!one) {
            if (err) *err = sh_strfmt("no agent %s", agent);
            return false;
        }
        if (strcmp(one->mode, "link") != 0) {
            if (err) *err = sh_strfmt("%s reads the shared folder: switch it with all agents that do", one->label);
            return false;
        }
    }
    if (one || all) {
        yyjson_mut_doc *m = toggles_read(err);
        if (!m) return false;
        for (size_t k = 0; k < st->n_agents; ++k) {
            const sh_agent *a = st->agents[k];
            if (strcmp(a->mode, "link") != 0 || (one && strcmp(a->dir, one->dir) != 0)) continue;  // agents that share a folder go together
            for (size_t i = 0; i < n; ++i) toggle_one(m, names[i], a->dir, on);
        }
        const bool ok = toggles_write(m);
        yyjson_mut_doc_free(m);
        if (!ok) {
            if (err) *err = sh_strdup("cannot write toggles.json");
            return false;
        }
    }
    bool ok = true;
    if (shared) ok = on ? unpark_many(st, names, n, err) : park_many(st, names, n, err);
    sh_store_relink(st);
    return ok;
}
