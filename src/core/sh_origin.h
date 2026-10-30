// Where skills come from: owner/repo on GitHub, a GitLab project (nested groups, self-hosted), any git URL (https, ssh,
// git@host:path), or a folder on this machine. Follows `npx skills` src/source-parser.ts, so both read the same inputs.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SH_ORIGIN_GITHUB, SH_ORIGIN_GITLAB, SH_ORIGIN_GIT, SH_ORIGIN_LOCAL } sh_origin_type;

typedef struct {
    sh_origin_type type;
    char *source;   // the lock file's "source": owner/repo, the repository URL, or the folder's absolute path
    char *url;      // what git clones (GitHub: https://github.com/owner/repo.git, unless an ssh URL was given); local: the folder
    char *ref;      // branch or tag; "" = the default branch
    char *subpath;  // the folder inside the repository that discovery is limited to; "" = all of it
    char *host;     // lower case; "" for a local folder
} sh_origin;

// Parses what the user typed (or an import file holds). false with *err when it is none of the above. An input that
// starts with '-' is refused, so it can never reach git as an option.
bool sh_origin_parse(const char *input, sh_origin *out, char **err);
// A branch or tag that is safe to hand to git, wherever it came from: never an option, no spaces or control characters.
bool sh_origin_ref_ok(const char *ref);
void sh_origin_free(sh_origin *o);
const char *sh_origin_type_name(sh_origin_type t);  // "github", "gitlab", "git", "local"
bool sh_origin_type_from_name(const char *name, sh_origin_type *t);
// The cache clone's folder (docs/formats.md, section 5): <cache>/repos/owner__repo for GitHub without a ref (the key
// 1.x used), else <cache>/repos/<host>/<path>-<8 hex of SHA-1 over normalized URL#ref>.
char *sh_origin_cache_dir(const sh_origin *o, const char *cache);

#ifdef __cplusplus
}
#endif
