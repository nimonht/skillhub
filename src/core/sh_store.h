// The skills store: one shared folder (~/.agents/skills) used by every coding agent, the agent catalog, the lock file
// shared with `npx skills`, updates from git, export and import, automation and uninstall. This is the C port of 1.x's
// hub::Store (src/store.cpp, src/providers.cpp, src/transfer.cpp); messages and decisions are kept word for word.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sh_str.h"
#include "yyjson.h"

#ifdef __cplusplus
extern "C" {
#endif

// A coding agent ("provider") that reads user-level skills.
typedef struct sh_agent {
    char *id;
    char *label;
    char *mode;          // link: one symlink per skill in `dir` | native: reads the shared folder | config: listed in a config file
    char *dir;           // where the agent reads user-level skills (native: the shared folder)
    char *project_dir;   // where it reads a project's skills, relative to the project; "" = unknown
    bool linked;         // mode == link
    sh_strv extra_dirs;  // other places it may keep skills (legacy folders): scanned for strays
    sh_strv ignore;      // entries owned by the agent itself, never touched
    sh_strv bins;        // commands that prove it is installed
    sh_strv detect;      // folders or files that prove it is installed
    char *config_file;   // config mode: the file that must list the shared folder
    bool detected;
    char *detected_by;   // "command `claude`" / "folder ~/.claude" / "added by you"
    bool managed;        // detected, unless the user decided otherwise
    bool custom;         // added by the user
} sh_agent;

typedef enum { SH_LINK_NATIVE, SH_LINK_LINKED, SH_LINK_MISSING, SH_LINK_COPY, SH_LINK_FOREIGN, SH_LINK_OFF, SH_LINK_SYNCED } sh_link_state;

typedef struct sh_source {
    char *type;         // sourceType: "github" (also when empty: 1.x wrote none), "gitlab", "git", "local", or one npx runs
    char *ref;          // branch or tag the skill follows ("" = the default branch)
    char *source;       // "owner/repo", the repository URL, or the local folder
    char *url;          // clone URL
    char *skill_path;   // path of SKILL.md inside the repository
    char *hash;         // git tree SHA of the installed folder ("" = unknown)
    char *installed_at;
    char *updated_at;
} sh_source;

typedef enum { SH_UPDATE_UNTRACKED, SH_UPDATE_UNKNOWN, SH_UPDATE_CURRENT, SH_UPDATE_AVAILABLE, SH_UPDATE_ERROR } sh_update;

typedef struct sh_skill {
    char *name;
    char *description;
    char *path;
    sh_source src;
    sh_link_state *links;  // one per managed agent, in the order of sh_store.agents
    size_t n_links;
    sh_update update;
    bool live;       // a link to a local folder: its edits show at once, and there is nothing to update
    char *new_hash;
    char *new_path;  // SKILL.md path upstream when the skill moved
    bool left_out;   // an automatic update passed over it (sh_engine_set_auto)
    bool parked;     // `path` is in the parked folder (sh_toggle.h)
    char *note;
} sh_skill;

typedef struct { sh_skill *items; size_t len; } sh_skills;

typedef struct sh_stray {
    char *agent_id;
    char *name;
    char *path;
    bool duplicate;  // the shared folder already holds a skill of that name
} sh_stray;

typedef struct { sh_stray *items; size_t len; } sh_strays;

typedef struct sh_candidate {
    char *name;
    char *description;
    char *dir;   // folder inside the repository
    char *slug;  // where it comes from: owner/repo, the repository URL, or the local folder
    char *url;
    char *key;   // the repository in the engine that found it
} sh_candidate;

typedef struct { sh_candidate *items; size_t len; } sh_candidates;

// A skill inside an export file. Imported files are untrusted: everything is validated before use.
typedef struct sh_import_file {
    char *path;
    bool exec;
    sh_buf data;
} sh_import_file;

typedef struct sh_import_item {
    char *name;
    char *description;
    bool local;               // the files are embedded; otherwise installed from a repository, over https
    char *source, *url, *skill_path;
    char *ref;                // branch or tag ("" = the default branch)
    sh_import_file *files;
    size_t n_files;
    long long bytes;
    bool exists;              // a skill of that name is installed already (it is skipped, never overwritten)
    char *problem;            // non-empty: rejected, with the reason
} sh_import_item;

typedef struct { sh_import_item *items; size_t len; } sh_import_items;

typedef void (*sh_log_fn)(void *user, const char *line);
struct sh_mutex;

typedef struct sh_store {
    char *home;
    char *project;        // the project this store works on, or NULL: the user's own skills
    char *scope;          // "user" or "project:<root>": the key in state.json and sources.json
    char *central;
    char *lock_path;      // the lock file new entries go to
    char *lock_other;     // a second lock file that also exists (docs/formats.md, section 2), or NULL
    char *backup;
    char *parked;         // skills switched off for every agent that reads the shared folder
    char *cache;
    char *state;          // durable internal state: the mutation lock, the swap journal
    sh_agent *providers;  // every agent in the catalog plus the user's own, with the detection result
    size_t n_providers;
    sh_agent **agents;    // the managed ones (detected, or switched on), pointing into `providers`, or `project_agents`
    size_t n_agents;
    sh_agent *project_agents;  // the same agents at their folders in the project
    sh_log_fn log;        // called from the engine's worker threads too, one line at a time
    void *log_user;
    struct sh_mutex *log_lock;
} sh_store;

void sh_store_init(sh_store *st, sh_log_fn log, void *user);
// A store of a project's skills (docs/formats.md, section 3): <root>/.agents/skills, skills-lock.json, and agents at their
// project folders. Everything else works as on the user's own skills. false with *err when `root` is no folder.
bool sh_store_init_project(sh_store *st, const char *root, sh_log_fn log, void *user, char **err);
void sh_store_free(sh_store *st);
void sh_store_log(sh_store *st, const char *fmt, ...) SH_PRINTF(2, 3);

// ---- agents (sh_catalog.c) ----
char *sh_expand_path(const char *s);  // "~", "${VAR:-default}" -> absolute path ("" stays "")
void sh_store_reload_providers(sh_store *st);
char *sh_store_providers_file(void);
bool sh_store_set_provider_managed(sh_store *st, const char *id, bool on, char **err);
char *sh_store_add_custom_provider(sh_store *st, const char *label, const char *dir, const char *mode, char **err);
bool sh_store_remove_custom_provider(sh_store *st, const char *id);
bool sh_store_provider_wired(const sh_store *st, const sh_agent *a);
bool sh_store_wiring_ok(const sh_store *st);
bool sh_store_ensure_provider_configs(sh_store *st, bool *changed);
void sh_store_watch_dirs(const sh_store *st, sh_strv *out);
bool sh_store_watcher_stale(const sh_store *st);
bool sh_store_sync_watcher(sh_store *st);
const sh_agent *sh_store_find_agent(const sh_store *st, const char *id);

// ---- lock file (sh_lock.c) ----
char *sh_lock_path_for(const char *home);  // which lock file to use (docs/formats.md, section 2)
void sh_lock_paths_for(const char *home, char **primary, char **other);
// Both lock files exist; *only_other: how many entries only the second one holds.
bool sh_lock_split(const sh_store *st, size_t *only_other);
// One-time merge of the second file into the first (backups of both are kept). `skillhub doctor --fix` runs it.
bool sh_lock_migrate(sh_store *st, char **err);
// A read-only view of the lock entries (both files when both exist; the newer entry wins), with version and skills
// ensured, or NULL with *err set when the lock file cannot be used.
yyjson_mut_doc *sh_lock_load(const sh_store *st, char **err);
void sh_source_from_json(yyjson_val *entry, sh_source *out);
void sh_source_from_mut_json(yyjson_mut_val *entry, sh_source *out);
void sh_source_free(sh_source *s);
void sh_source_copy(const sh_source *from, sh_source *to);
bool sh_source_tracked(const sh_source *s);
bool sh_source_runnable(const sh_source *s);  // a type Skill Hub can update (the others are managed by npx)
// Sets (src non-NULL) or removes the entry `name`, re-reading and merging so other tools' entries survive.
bool sh_lock_put(sh_store *st, const char *name, const sh_source *src);
// The same for many entries at once: each lock file is read and written once (the engine's group commits).
bool sh_lock_put_many(sh_store *st, const char *const *names, const sh_source *const *srcs, size_t n);
// Below the entries of parked skills are told apart too: they live in state.json (sh_toggle.h). This writes the lock files alone.
bool sh_lock_files_put(sh_store *st, const char *const *names, const sh_source *const *srcs, size_t n);
bool sh_lock_entry(const sh_store *st, const char *name, sh_source *out);  // false when the lock has no such entry
yyjson_mut_val *sh_source_to_json(yyjson_mut_doc *m, const sh_source *src);
void sh_source_resolve(const sh_store *st, sh_source *s);  // a project lock holds local sources relative to the project

// <cache>/sources.json: update data of Skill Hub's own (docs/formats.md, section 5). For GitLab and git sources the lock
// holds npx's SHA-256 folder hash, which cannot be compared with upstream without downloading it; the git tree id each
// skill was installed from is kept here instead, and for every source the SHA-256 folder hash of what was placed, which
// tells local edits apart. Derived data: when it is lost, the next update restores it.
typedef struct sh_trees sh_trees;
sh_trees *sh_trees_load(const sh_store *st);
const char *sh_trees_get(const sh_trees *t, const char *name);      // "" when unknown
const char *sh_trees_content(const sh_trees *t, const char *name);  // "" when unknown
void sh_trees_free(sh_trees *t);
void sh_trees_put_many(sh_store *st, const char *const *names, const char *const *ids, const char *const *contents, size_t n);

// ---- skills (sh_store.c) ----
void sh_store_scan(const sh_store *st, sh_skills *out);
void sh_store_strays(const sh_store *st, sh_strays *out);
int sh_store_adopt(sh_store *st);   // pull stray skills out of agent folders into the shared folder
int sh_store_relink(sh_store *st);  // point every link-mode agent at the shared folder, drop dead links
bool sh_store_remove_skill(sh_store *st, const char *name);
char *sh_store_skill_dir(const sh_store *st, const char *name);  // where the skill sits: shared folder, or parked
// Set the upstream of a local skill (sh_engine.c: it reads the repository to find the skill by name).
bool sh_store_track(sh_store *st, const char *name, const char *repo_input, const char *path, char **err);
char *sh_dir_of(const char *skill_md_path);
// A fresh staging folder name for `label`, unique across threads and processes (the folder is not created).
char *sh_store_stage_dir(const sh_store *st, const char *label);
// What is written next to a backup (docs/formats.md, section 4).
typedef struct {
    const char *reason;      // update, install, rollback, remove
    const sh_source *entry;  // the lock entry of the version being replaced; NULL when it has none
    const char *tree;        // the upstream tree id that version came from; "" when unknown
} sh_backup_note;
// Checks a staged skill folder (a SKILL.md with a description) and swaps it in as skill `name`, keeping the folder it
// replaces as a backup, described by `note` (may be NULL). The stage is gone afterwards either way. Safe from worker threads
// for different names.
bool sh_store_place(sh_store *st, const char *name, const char *stage, const sh_backup_note *note);
// The same for a live link: skill `name` becomes a link to the folder `target`, whose edits every agent then sees at once.
// The folder itself is never moved or changed.
bool sh_store_place_link(sh_store *st, const char *name, const char *target, const sh_backup_note *note);
// The same for a backup coming back: on failure it stays where it is.
bool sh_store_put_back(sh_store *st, const char *name, const char *backup, const sh_backup_note *note);
// Cleans up after a process that died while changing skills (docs/formats.md, section 8): puts back a folder an
// interrupted swap had moved aside, removes staging folders, and (`git_locks`) removes git lock files older than ten
// minutes from cache clones nobody is using. Only call it while holding the mutation lock. Returns the folders put back.
int sh_store_recover(sh_store *st, bool git_locks);

void sh_skills_free(sh_skills *s);
void sh_strays_free(sh_strays *s);
void sh_candidates_free(sh_candidates *c);
void sh_candidate_free(sh_candidate *c);

// ---- moving skills between machines (sh_transfer.c) ----
bool sh_store_write_export(sh_store *st, const char *path, bool include_local, int *tracked, int *local, sh_strv *skipped,
                           char **err);
void sh_store_read_export(const sh_store *st, const char *path, sh_import_items *out, char **err);  // validates, never writes
void sh_store_read_export_data(const sh_store *st, const char *data, size_t n, sh_import_items *out, char **err);
// Installs the chosen skills of an export file: embedded ones from the file, GitHub ones all at once through the engine.
// A skill of a name that is installed already is skipped, never overwritten.
void sh_store_import_items(sh_store *st, const sh_import_item *const *items, size_t n, int *ok, int *failed);
void sh_import_items_free(sh_import_items *items);

// ---- automation and uninstall (sh_units.c) ----
bool sh_automation_supported(void);
const char *sh_automation_mechanism(void);        // systemd, launchd, Task Scheduler or none
const char *sh_automation_name(bool timer);       // the unit, agent or task, as shown to the user
char *sh_automation_state(const sh_store *st, bool timer);  // "enabled", "disabled", "not installed" or "not available"
void sh_automation_apply_env(void);               // scheduled runs: the variables recorded when automation was switched on
bool sh_store_install_units(sh_store *st, bool timer, bool watcher, char **err);
bool sh_store_remove_units(sh_store *st, bool timer, bool watcher, char **err);
bool sh_store_uninstall(sh_store *st, bool dry_run, bool keep_cache);
void sh_notify(const char *title, const char *body);  // desktop notification, best effort

#ifdef __cplusplus
}
#endif
