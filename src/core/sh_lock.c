// The user lock file shared with `npx skills` (docs/formats.md, section 2). npx keeps it in $XDG_STATE_HOME/skills when that
// variable is set, Skill Hub 1.x always kept it in ~/.agents; a machine can have both. Both are then read, a change goes to
// the file (or files) that already hold that entry, and nothing is ever copied from one file to the other except by the
// one-time migration that `skillhub doctor --fix` runs. Files that cannot be parsed are never overwritten.
#include <stdlib.h>
#include <string.h>

#include "sh_fs.h"
#include "sh_json.h"
#include "sh_path.h"
#include "sh_store.h"
#include "sh_time.h"

void sh_lock_paths_for(const char *home, char **primary, char **other) {
    char *legacy = sh_path_join(home, ".agents/.skill-lock.json");
    *other = NULL;
    char *forced = sh_getenv("SKILLHUB_HOME");
    const bool pinned = forced && *forced;
    free(forced);
    char *state = sh_getenv("XDG_STATE_HOME");
    if (pinned || !state || state[0] != '/') {  // tests and portable setups, or npx uses the legacy file too
        free(state);
        *primary = legacy;
        return;
    }
    char *clean = sh_path_clean(state);
    char *xdg = sh_path_join(clean, "skills/.skill-lock.json");
    free(clean);
    free(state);
    const bool have_legacy = sh_lexists(legacy), have_xdg = sh_lexists(xdg);
    if (have_legacy && !have_xdg) {  // only the file 1.x wrote: keep using it, never switch silently
        free(xdg);
        *primary = legacy;
        return;
    }
    *primary = xdg;  // new entries go where npx looks
    if (have_legacy) *other = legacy;  // both exist: the legacy one is still read, and kept up to date for its own entries
    else free(legacy);
}

char *sh_lock_path_for(const char *home) {
    char *primary, *other;
    sh_lock_paths_for(home, &primary, &other);
    free(other);
    return primary;
}

void sh_source_free(sh_source *s) {
    free(s->source);
    free(s->url);
    free(s->skill_path);
    free(s->hash);
    free(s->installed_at);
    free(s->updated_at);
    memset(s, 0, sizeof *s);
}

void sh_source_copy(const sh_source *from, sh_source *to) {
    to->source = sh_strdup(from->source);
    to->url = sh_strdup(from->url);
    to->skill_path = sh_strdup(from->skill_path);
    to->hash = sh_strdup(from->hash);
    to->installed_at = sh_strdup(from->installed_at);
    to->updated_at = sh_strdup(from->updated_at);
}

bool sh_source_tracked(const sh_source *s) { return s->source && *s->source; }

void sh_source_from_json(yyjson_val *o, sh_source *out) {
    out->source = sh_strdup(sh_json_str(o, "source"));
    out->url = sh_strdup(sh_json_str(o, "sourceUrl"));
    out->skill_path = sh_strdup(sh_json_str(o, "skillPath"));
    out->hash = sh_strdup(sh_json_str(o, "skillFolderHash"));
    out->installed_at = sh_strdup(sh_json_str(o, "installedAt"));
    out->updated_at = sh_strdup(sh_json_str(o, "updatedAt"));
}

// One lock file as a mutable document with `version` and `skills` present, or NULL with *err when it must not be used.
static yyjson_mut_doc *load_file(const char *path, char **err) {
    yyjson_doc *doc = NULL;
    sh_json_status s = sh_json_read(path, 0, &doc);
    if (s == SH_JSON_INVALID || s == SH_JSON_UNREADABLE) {  // npx writes in place: we may have read half a file
        sh_sleep_ms(200);
        s = sh_json_read(path, 0, &doc);
    }
    if (s == SH_JSON_INVALID || s == SH_JSON_UNREADABLE) {
        if (err) *err = sh_strfmt("%s cannot be read; it is left as it is until it is fixed", path);
        return NULL;
    }
    yyjson_mut_doc *m;
    yyjson_val *skills = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "skills") : NULL;
    if (skills && !yyjson_is_obj(skills)) {  // "skills" that is not an object: not a lock file we can safely rewrite
        if (err) *err = sh_strfmt("%s cannot be read; it is left as it is until it is fixed", path);
        yyjson_doc_free(doc);
        return NULL;
    }
    if (doc) {
        const long long version = sh_json_int(yyjson_doc_get_root(doc), "version", 3);
        if (version > 3) {
            if (err) *err = sh_strfmt("%s was written by a newer tool (version %lld); it is left as it is", path, version);
            yyjson_doc_free(doc);
            return NULL;
        }
        m = yyjson_doc_mut_copy(doc, NULL);
        yyjson_doc_free(doc);
    } else {
        m = yyjson_mut_doc_new(NULL);
        yyjson_mut_doc_set_root(m, yyjson_mut_obj(m));
    }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(m);
    if (!yyjson_mut_obj_get(root, "version")) sh_json_set_int(m, root, "version", 3);
    sh_json_get_or_add_obj(m, root, "skills");
    return m;
}

static yyjson_mut_val *skills_of(yyjson_mut_doc *m) { return yyjson_mut_obj_get(yyjson_mut_doc_get_root(m), "skills"); }

static const char *updated_at(yyjson_mut_val *entry) {
    const char *s = yyjson_mut_get_str(yyjson_mut_obj_get(entry, "updatedAt"));
    return s ? s : "";
}

yyjson_mut_doc *sh_lock_load(const sh_store *st, char **err) {
    yyjson_mut_doc *view = load_file(st->lock_path, err);
    if (!view || !st->lock_other) return view;
    yyjson_mut_doc *other = load_file(st->lock_other, NULL);  // the second file only adds to the view; a broken one is skipped
    if (!other) return view;
    yyjson_mut_val *mine = skills_of(view);
    size_t i, n;
    yyjson_mut_val *key, *val;
    yyjson_mut_obj_foreach(skills_of(other), i, n, key, val) {
        const char *name = yyjson_mut_get_str(key);
        yyjson_mut_val *have = yyjson_mut_obj_get(mine, name);
        // an entry in both files: the newer one counts (ISO timestamps compare as text)
        if (!have || strcmp(updated_at(val), updated_at(have)) > 0)
            sh_json_set_val(view, mine, name, yyjson_mut_val_mut_copy(view, val));
    }
    yyjson_mut_doc_free(other);
    return view;
}

static void set_entry(yyjson_mut_doc *m, const char *name, const sh_source *src) {
    yyjson_mut_val *skills = skills_of(m);
    yyjson_mut_val *entry = yyjson_mut_obj_get(skills, name);
    if (!yyjson_mut_is_obj(entry)) {
        entry = yyjson_mut_obj(m);
        sh_json_set_val(m, skills, name, entry);
    }
    // only our fields change; fields other tools wrote stay
    sh_json_set_str(m, entry, "source", src->source);
    sh_json_set_str(m, entry, "sourceType", "github");
    sh_json_set_str(m, entry, "sourceUrl", src->url);
    sh_json_set_str(m, entry, "skillPath", src->skill_path);
    sh_json_set_str(m, entry, "skillFolderHash", src->hash);
    sh_json_set_str(m, entry, "installedAt", src->installed_at);
    sh_json_set_str(m, entry, "updatedAt", src->updated_at);
}

static bool write_lock(sh_store *st, const char *path, yyjson_mut_doc *m) {
    const bool ok = sh_json_write(path, m, true, false);  // as npx writes it: two spaces, no final newline
    if (!ok) sh_store_log(st, "FAILED to write %s", path);
    return ok;
}

bool sh_lock_put(sh_store *st, const char *name, const sh_source *src) {
    char *err = NULL;
    yyjson_mut_doc *primary = load_file(st->lock_path, &err);
    if (!primary) {
        sh_store_log(st, "FAILED to record %s: %s", name, err);
        free(err);
        return false;
    }
    yyjson_mut_doc *other = st->lock_other ? load_file(st->lock_other, NULL) : NULL;
    const bool in_primary = yyjson_mut_obj_get(skills_of(primary), name) != NULL;
    const bool in_other = other && yyjson_mut_obj_get(skills_of(other), name) != NULL;
    bool ok = true;
    if (!src) {  // removed: gone from both files
        if (in_primary) {
            yyjson_mut_obj_remove_str(skills_of(primary), name);
            ok = write_lock(st, st->lock_path, primary) && ok;
        }
        if (in_other) {
            yyjson_mut_obj_remove_str(skills_of(other), name);
            ok = write_lock(st, st->lock_other, other) && ok;
        }
    } else {
        // the files that hold the entry get the change; a new entry goes to the primary file
        if (in_primary || !in_other) {
            set_entry(primary, name, src);
            ok = write_lock(st, st->lock_path, primary) && ok;
        }
        if (in_other) {
            set_entry(other, name, src);
            ok = write_lock(st, st->lock_other, other) && ok;
        }
    }
    yyjson_mut_doc_free(primary);
    yyjson_mut_doc_free(other);
    return ok;
}

bool sh_lock_split(const sh_store *st, size_t *only_other) {
    if (only_other) *only_other = 0;
    if (!st->lock_other) return false;
    yyjson_mut_doc *primary = load_file(st->lock_path, NULL), *other = load_file(st->lock_other, NULL);
    if (primary && other && only_other) {
        size_t i, n;
        yyjson_mut_val *key, *val;
        yyjson_mut_obj_foreach(skills_of(other), i, n, key, val)
            if (!yyjson_mut_obj_get(skills_of(primary), yyjson_mut_get_str(key))) ++*only_other;
    }
    yyjson_mut_doc_free(primary);
    yyjson_mut_doc_free(other);
    return true;
}

bool sh_lock_migrate(sh_store *st, char **err) {
    if (!st->lock_other) return true;
    yyjson_mut_doc *primary = load_file(st->lock_path, err);
    if (!primary) return false;
    yyjson_mut_doc *other = load_file(st->lock_other, err);
    if (!other) {
        yyjson_mut_doc_free(primary);
        return false;
    }
    char *stamp = sh_stamp();
    char *backup_primary = sh_strfmt("%s.bak-%s", st->lock_path, stamp);
    char *backup_other = sh_strfmt("%s.bak-%s", st->lock_other, stamp);
    bool ok = sh_copy_file(st->lock_path, backup_primary);
    int moved = 0;
    if (ok) {
        size_t i, n;
        yyjson_mut_val *key, *val;
        yyjson_mut_obj_foreach(skills_of(other), i, n, key, val) {
            const char *name = yyjson_mut_get_str(key);
            char *folder = sh_path_join(st->central, name);
            const bool installed = sh_is_dir(folder);
            free(folder);
            if (installed && !yyjson_mut_obj_get(skills_of(primary), name)) {
                sh_json_set_val(primary, skills_of(primary), name, yyjson_mut_val_mut_copy(primary, val));
                ++moved;
            }
        }
        ok = write_lock(st, st->lock_path, primary) && sh_rename(st->lock_other, backup_other);
    }
    if (ok) {
        sh_store_log(st, "merged %d entries from %s into %s (backups: %s, %s)", moved, st->lock_other, st->lock_path,
                     backup_other, backup_primary);
        // remembered in state.json, so the window does not offer the merge again
        char *config = sh_config_dir();
        char *state_file = sh_path_join(config, "state.json");
        yyjson_doc *doc = NULL;
        const sh_json_status s = sh_json_read(state_file, 0, &doc);
        if (s == SH_JSON_OK || s == SH_JSON_MISSING) {
            yyjson_mut_doc *m = doc ? yyjson_doc_mut_copy(doc, NULL) : yyjson_mut_doc_new(NULL);
            if (!doc) yyjson_mut_doc_set_root(m, yyjson_mut_obj(m));
            yyjson_mut_val *root = yyjson_mut_doc_get_root(m);
            sh_json_set_int(m, root, "version", 1);
            yyjson_mut_val *mig = yyjson_mut_obj(m);
            char *now = sh_now_iso();
            sh_json_set_str(m, mig, "from", st->lock_other);
            sh_json_set_str(m, mig, "to", st->lock_path);
            sh_json_set_str(m, mig, "at", now);
            sh_json_set_int(m, mig, "entries", moved);
            sh_json_set_val(m, root, "lockMigration", mig);
            sh_json_write(state_file, m, true, true);
            free(now);
            yyjson_mut_doc_free(m);
        }
        yyjson_doc_free(doc);
        free(state_file);
        free(config);
        free(st->lock_other);
        st->lock_other = NULL;
    } else if (err && !*err) {
        *err = sh_strfmt("could not merge %s into %s", st->lock_other, st->lock_path);
    }
    free(backup_primary);
    free(backup_other);
    free(stamp);
    yyjson_mut_doc_free(primary);
    yyjson_mut_doc_free(other);
    return ok;
}
