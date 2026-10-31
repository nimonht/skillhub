// Earlier versions of a skill: the backups with their sidecars, rollback, bringing back a removed skill, and holds, which
// keep automatic updates away from a version the user rolled back from (docs/formats.md, sections 4 and 6).
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sh_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *stamp;       // the backup's folder name: when it was made, and its id for rollback
    char *reason;      // update, install, rollback; "" for a backup from an older version
    char *created_at;  // ISO UTC; "" for a backup from an older version
    char *tree;        // the upstream tree id of that version; "" when unknown
    bool has_entry;    // its lock entry was kept, so a rollback restores the source too
} sh_version;

typedef struct { sh_version *items; size_t len; } sh_versions;

void sh_history_list(const sh_store *st, const char *name, sh_versions *out);  // newest first
void sh_versions_free(sh_versions *v);
// Puts a kept version back (`stamp` NULL or "": the newest) after backing up the current one, restores its lock entry and
// sets a hold. Without a sidecar only the files come back and the hash is cleared, so the next check offers the update.
bool sh_history_rollback(sh_store *st, const char *name, const char *stamp, char **err);
// Brings back the skill most recently removed under that name.
bool sh_history_restore(sh_store *st, const char *name, char **err);

typedef struct sh_holds sh_holds;
sh_holds *sh_holds_load(const sh_store *st);
// NULL when `name` is not held; otherwise the upstream tree it is held from ("" = whatever upstream does).
const char *sh_holds_get(const sh_holds *h, const char *name);
void sh_holds_free(sh_holds *h);
bool sh_hold_set(sh_store *st, const char *name, const char *from_tree, char **err);
bool sh_holds_release(sh_store *st, const char *const *names, size_t n);

#ifdef __cplusplus
}
#endif
