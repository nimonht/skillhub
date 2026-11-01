// What Skill Hub keeps besides the skills themselves, and how that stays a reasonable size (docs/formats.md, section 4 and 5).
// The cache can always be downloaded again, so it has a limit in bytes. Backups can be the only copy of something, so they are
// never cleaned by size: each skill keeps its newest versions, and only the loose folders age out.
#pragma once

#include <stdint.h>

#include "sh_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int64_t cache;      // cloned repositories
    int64_t versions;   // older versions of skills; the newest of each is never cleaned, so it is not counted
    int64_t removed;    // removed skills, duplicates and replaced parked copies
    int64_t conflicts;  // copies that differed from the shared folder: the user's own edits
} sh_storage;

enum { SH_CLEAN_CACHE = 1, SH_CLEAN_VERSIONS = 2, SH_CLEAN_REMOVED = 4, SH_CLEAN_CONFLICTS = 8 };

// What sh_storage_clean would free, in each of its categories.
void sh_storage_measure(const sh_store *st, sh_storage *out);

// Cleans the categories in `what`; returns the bytes freed. Takes the mutation lock's protection from its caller.
int64_t sh_storage_clean(sh_store *st, int what);

// The automatic part, after a job that may have added to the cache or the backups: the cache limit, oldest clones first,
// and the age limit of the loose backups. Conflicts stay: only a person asking can remove them.
void sh_storage_tidy(sh_store *st);

#ifdef __cplusplus
}
#endif
