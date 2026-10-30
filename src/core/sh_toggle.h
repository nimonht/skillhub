// Which agents see a skill (docs/formats.md, sections 6 and 7). Agents with a folder of their own are switched one by one, in
// toggles.json. Agents that read the shared folder can only be told apart from it by moving the skill out: it is parked in
// ~/.agents/.parked, and its lock entry moves to state.json, because `npx skills update -g` would otherwise bring it back.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sh_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sh_toggles sh_toggles;
sh_toggles *sh_toggles_load(void);
bool sh_toggles_off(const sh_toggles *t, const char *name, const char *agent_dir);
void sh_toggles_free(sh_toggles *t);
void sh_toggles_forget(const char *name);  // the skill is gone

#define SH_AGENTS_SHARED "@shared"  // every agent that reads the shared folder
#define SH_AGENTS_ALL "@all"

// Switches skills for `agent`: an agent id, SH_AGENTS_SHARED or SH_AGENTS_ALL. All skills at once: one write of each file, one
// relink. An agent that reads the shared folder stands for all of them, and `err` says so.
bool sh_skills_set_enabled(sh_store *st, const char *const *names, size_t n, const char *agent, bool on, char **err);
int sh_unpark_all(sh_store *st);  // before going back to 1.x, which knows nothing of parked skills

bool sh_store_is_parked(const sh_store *st, const char *name);
bool sh_parked_entry(const sh_store *st, const char *name, sh_source *out);
void sh_parked_put(sh_store *st, const char *name, const sh_source *src);
void sh_parked_forget(sh_store *st, const char *name);
// After a crash, or when npx installed the same name meanwhile: the shared folder wins, and the parked copy becomes a backup.
void sh_parked_reconcile(sh_store *st);

#ifdef __cplusplus
}
#endif
