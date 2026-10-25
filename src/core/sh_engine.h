// The engine: store operations that need git, run over many skills at once. Skills are grouped by repository and each
// repository is one task on a worker thread: brought up to date at most once per engine (an `ls-remote` decides whether
// anything is downloaded at all), read with one listing of its tree, and its skill folders extracted through one git
// process. Worker threads never write the lock file: finished entries are committed in groups from the calling thread,
// and link-mode agents are relinked once at the end of a job instead of after every skill.
#pragma once

#include "sh_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sh_engine sh_engine;

// One engine per job; it remembers the repositories it has brought up to date, so check then update fetch only once.
sh_engine *sh_engine_new(sh_store *st);
void sh_engine_free(sh_engine *e);

// Stops the job at its next safe point: no further repository is started and no further skill is swapped in. Skills
// swapped in already stay, and their lock entries are committed. Callable from any thread.
void sh_engine_cancel(sh_engine *e);
bool sh_engine_cancelled(sh_engine *e);
// Called from the worker threads each time a repository is done: `done` of `total` skills are through, `current` is the
// repository that just finished.
typedef void (*sh_engine_progress)(void *user, size_t done, size_t total, const char *current);
void sh_engine_on_progress(sh_engine *e, sh_engine_progress fn, void *user);

// Sets the update state of every tracked skill (`only`: just these names; NULL or empty: all). Returns how many have an
// update.
int sh_engine_check(sh_engine *e, sh_skills *skills, const sh_strv *only);
// Applies the updates sh_engine_check found to the skills in `todo`; done[i] tells which ones worked. Returns how many did.
int sh_engine_update(sh_engine *e, sh_skill *const *todo, size_t n, bool *done);

typedef struct {
    sh_candidates cands;
    char *err;  // why there are none
} sh_discovery;
// The skills each input (owner/repo or a github.com URL) offers, all repositories at once.
void sh_engine_discover(sh_engine *e, const char *const *inputs, size_t n, sh_discovery *out);
void sh_discovery_free(sh_discovery *d);
// Installs candidates, each name once (callers drop duplicates first); done[i] tells which ones worked.
int sh_engine_install(sh_engine *e, const sh_candidate *const *cands, size_t n, bool *done);

// The skills of an import file that come from GitHub (checked by the caller before). A skill whose folder moved upstream
// is found by name. why[i] is set for each one that failed; done[i] tells which ones worked.
typedef enum { SH_IMPORT_OK, SH_IMPORT_UNREACHABLE, SH_IMPORT_GONE, SH_IMPORT_FAILED } sh_import_why;
int sh_engine_import(sh_engine *e, const sh_import_item *const *items, size_t n, bool *done, sh_import_why *why,
                     char **errs);

#ifdef __cplusplus
}
#endif
