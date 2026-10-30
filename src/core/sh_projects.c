#include "sh_projects.h"

#include <stdlib.h>
#include <string.h>

#include "sh_fs.h"
#include "sh_json.h"
#include "sh_path.h"
#include "sh_store.h"
#include "sh_time.h"

#define SCAN_DEPTH 3

static char *projects_path(void) {
    char *config = sh_config_dir();
    char *p = sh_path_join(config, "projects.json");
    free(config);
    return p;
}

// the file as a document with `projects` present; NULL when it exists and cannot be read: it is then never overwritten
static yyjson_mut_doc *read_doc(char **err) {
    char *path = projects_path();
    yyjson_doc *doc = NULL;
    const sh_json_status s = sh_json_read(path, 0, &doc);
    free(path);
    if (s != SH_JSON_OK && s != SH_JSON_MISSING) {
        if (err) *err = sh_strdup("projects.json cannot be read");
        return NULL;
    }
    yyjson_mut_doc *m = doc ? yyjson_doc_mut_copy(doc, NULL) : yyjson_mut_doc_new(NULL);
    yyjson_doc_free(doc);
    if (!yyjson_mut_is_obj(yyjson_mut_doc_get_root(m))) yyjson_mut_doc_set_root(m, yyjson_mut_obj(m));
    yyjson_mut_val *root = yyjson_mut_doc_get_root(m);
    if (!yyjson_mut_obj_get(root, "version")) sh_json_set_int(m, root, "version", 1);
    if (!yyjson_mut_is_arr(yyjson_mut_obj_get(root, "projects"))) sh_json_set_val(m, root, "projects", yyjson_mut_arr(m));
    return m;
}

static yyjson_mut_val *list_of(yyjson_mut_doc *m) { return yyjson_mut_obj_get(yyjson_mut_doc_get_root(m), "projects"); }

static bool write_doc(yyjson_mut_doc *m, char **err) {
    char *path = projects_path();
    const bool ok = sh_json_write(path, m, true, true);
    free(path);
    if (!ok && err) *err = sh_strdup("cannot write projects.json");
    return ok;
}

bool sh_projects_load(sh_projects *out, char **err) {
    memset(out, 0, sizeof *out);
    yyjson_mut_doc *m = read_doc(err);
    if (!m) return false;
    yyjson_mut_val *list = list_of(m);
    out->items = sh_xcalloc(yyjson_mut_arr_size(list) + 1, sizeof *out->items);
    size_t i, n;
    yyjson_mut_val *o;
    yyjson_mut_arr_foreach(list, i, n, o) {
        if (!yyjson_mut_is_obj(o) || !*sh_json_mut_str(o, "path")) continue;
        sh_project *p = &out->items[out->len++];
        p->path = sh_strdup(sh_json_mut_str(o, "path"));
        p->name = sh_strdup(sh_json_mut_str(o, "name"));
        p->auto_update = yyjson_mut_get_bool(yyjson_mut_obj_get(o, "autoUpdate"));
    }
    yyjson_mut_doc_free(m);
    return true;
}

void sh_projects_free(sh_projects *p) {
    for (size_t i = 0; i < p->len; ++i) {
        free(p->items[i].path);
        free(p->items[i].name);
    }
    free(p->items);
    memset(p, 0, sizeof *p);
}

static yyjson_mut_val *find(yyjson_mut_val *list, const char *path, size_t *at) {
    size_t i, n;
    yyjson_mut_val *o;
    yyjson_mut_arr_foreach(list, i, n, o) {
        if (!yyjson_mut_is_obj(o) || strcmp(sh_json_mut_str(o, "path"), path) != 0) continue;
        if (at) *at = i;
        return o;
    }
    return NULL;
}

// a typed path as it is on record: links resolved, and only then compared
static char *key_of(const char *path) {
    char *full = sh_canonical(path);
    return full ? full : sh_strdup(path);
}

bool sh_projects_add(const char *path, char **err) {
    char *root = sh_canonical(path);
    if (!root || !sh_is_dir(root)) {
        if (err) *err = sh_strfmt("%s is not a folder", path);
        free(root);
        return false;
    }
    yyjson_mut_doc *m = read_doc(err);
    bool ok = m != NULL;
    if (ok && !find(list_of(m), root, NULL)) {
        yyjson_mut_val *o = yyjson_mut_obj(m);
        sh_json_set_str(m, o, "path", root);
        const char *slash = strrchr(root, '/');
        sh_json_set_str(m, o, "name", slash ? slash + 1 : root);
        char *now = sh_now_iso();
        sh_json_set_str(m, o, "addedAt", now);
        free(now);
        yyjson_mut_obj_add_bool(m, o, "autoUpdate", false);
        yyjson_mut_arr_append(list_of(m), o);
        ok = write_doc(m, err);
    }
    yyjson_mut_doc_free(m);
    free(root);
    return ok;
}

bool sh_projects_remove(const char *path, char **err) {
    char *key = key_of(path);
    yyjson_mut_doc *m = read_doc(err);
    bool ok = m != NULL;
    size_t at = 0;
    if (ok && find(list_of(m), key, &at)) {
        yyjson_mut_arr_remove(list_of(m), at);
        ok = write_doc(m, err);
    } else if (ok) {
        if (err) *err = sh_strfmt("%s is not listed", path);
        ok = false;
    }
    yyjson_mut_doc_free(m);
    free(key);
    return ok;
}

bool sh_projects_set_auto(const char *path, bool on, char **err) {
    char *key = key_of(path);
    yyjson_mut_doc *m = read_doc(err);
    yyjson_mut_val *o = m ? find(list_of(m), key, NULL) : NULL;
    bool ok = o != NULL;
    if (ok) {
        yyjson_mut_obj_remove_str(o, "autoUpdate");
        yyjson_mut_obj_add_bool(m, o, "autoUpdate", on);
        ok = write_doc(m, err);
    } else if (m && err) {
        *err = sh_strfmt("%s is not listed", path);
    }
    yyjson_mut_doc_free(m);
    free(key);
    return ok;
}

static bool holds_skills(const char *dir) {
    static const char *const marks[] = {".agents/skills", ".claude/skills", "skills-lock.json"};
    for (size_t i = 0; i < sizeof marks / sizeof marks[0]; ++i) {
        char *p = sh_path_join(dir, marks[i]);
        const bool yes = sh_exists(p);
        free(p);
        if (yes) return true;
    }
    return false;
}

static void walk_root(const char *dir, int depth, const sh_projects *known, sh_strv *out) {
    if (holds_skills(dir)) {
        bool listed = false;
        for (size_t i = 0; i < known->len && !listed; ++i) listed = strcmp(known->items[i].path, dir) == 0;
        if (!listed && !sh_strv_contains(out, dir)) sh_strv_push(out, dir);
        return;
    }
    if (depth == 0) return;
    sh_strv names = SH_STRV_INIT;
    sh_list_dir(dir, SH_LIST_DIRS | SH_LIST_NO_SYMLINKS | SH_SORT_NAME, &names);
    for (size_t i = 0; i < names.len; ++i) {
        if (names.items[i][0] == '.' || strcmp(names.items[i], "node_modules") == 0) continue;
        char *child = sh_path_join(dir, names.items[i]);
        walk_root(child, depth - 1, known, out);
        free(child);
    }
    sh_strv_free(&names);
}

static void scan_roots(sh_strv *roots) {
    char *config = sh_config_dir();
    char *path = sh_path_join(config, "settings.json");
    yyjson_doc *doc = NULL;
    if (sh_json_read(path, 0, &doc) == SH_JSON_OK) {
        yyjson_val *projects = yyjson_obj_get(yyjson_doc_get_root(doc), "projects");
        yyjson_val *list = projects ? yyjson_obj_get(projects, "scanRoots") : NULL;
        size_t i, n;
        yyjson_val *v;
        if (yyjson_is_arr(list)) {
            yyjson_arr_foreach(list, i, n, v) {
                if (!yyjson_is_str(v)) continue;
                char *full = sh_expand_path(yyjson_get_str(v));
                sh_strv_push(roots, full);
                free(full);
            }
        }
        yyjson_doc_free(doc);
    }
    free(path);
    free(config);
    if (roots->len == 0) {
        char *fallback = sh_expand_path("~/Projects");
        sh_strv_push(roots, fallback);
        free(fallback);
    }
}

void sh_projects_suggest(sh_strv *out) {
    sh_projects known;
    if (!sh_projects_load(&known, NULL)) return;
    sh_strv roots = SH_STRV_INIT;
    scan_roots(&roots);
    for (size_t i = 0; i < roots.len; ++i) {
        char *root = sh_canonical(roots.items[i]);
        if (root && sh_is_dir(root)) walk_root(root, SCAN_DEPTH, &known, out);
        free(root);
    }
    sh_strv_free(&roots);
    sh_projects_free(&known);
}
