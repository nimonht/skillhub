// The SHA-256 folder hash (`computedHash`, docs/formats.md section 3) that npx writes for git, GitLab and local sources
// and for project locks, and JavaScript's localeCompare order that it sorts files by.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int sh_locale_compare(const char *a, const char *b);  // like JavaScript's a.localeCompare(b) with no locale
char *sh_folder_hash(const char *dir);                // 64 hex digits; .git and node_modules folders and links are skipped
bool sh_folder_hash_skips(const char *rel);           // a file below such a folder

#ifdef __cplusplus
}
#endif
