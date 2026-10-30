// git as the engine uses it: one cache clone per upstream repository, brought up to date at most once per job (an
// `ls-remote` first, so an unchanged repository downloads nothing), one listing of the commit's whole tree, many objects
// read through a single `git cat-file --batch`, and skill folders written to disk while git streams them.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sh_str.h"
#include "sh_thread.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool ok;
    sh_buf out;
    char *err;  // trimmed stderr, or why git could not run
} sh_git_result;

// git with the options every call gets. work_dir NULL: the current folder; timeout_ms 0: three minutes.
sh_git_result sh_git_run(const char *const *args, const char *work_dir, int timeout_ms);
void sh_git_result_free(sh_git_result *r);

#define SH_MODE_TREE 0040000u
#define SH_MODE_FILE 0100644u
#define SH_MODE_EXEC 0100755u

typedef struct {
    const char *path;  // '/'-separated, from the top of the repository
    const char *id;    // object id
    unsigned mode;     // SH_MODE_TREE, SH_MODE_FILE, SH_MODE_EXEC, 0120000 link, 0160000 submodule
} sh_tree_entry;

// A cache clone at the commit it points to, with that commit's whole tree.
typedef struct {
    char *dir;
    char *head;              // commit id
    char *root;              // tree id of the commit
    sh_tree_entry *entries;  // every entry, folders too, sorted by path in byte order
    size_t n;
    sh_buf listing;          // the ls-tree output the entries point into
} sh_repo;

bool sh_repo_cloned(const char *dir);  // a cache clone exists there
// Clones `url` into `dir`, or brings the clone up to date when upstream has moved, then reads the commit's tree. `ref` is
// a branch or tag ("": upstream's default). Other processes using the same clone wait (`dir`.lock). `fetches` (may be
// NULL) bounds clones and fetches per host. On failure *err holds git's own message.
bool sh_repo_open(sh_repo *r, const char *dir, const char *url, const char *ref, sh_limiter *fetches, char **err);
void sh_repo_free(sh_repo *r);
const sh_tree_entry *sh_repo_find(const sh_repo *r, const char *path);  // NULL when the commit has no such path
const char *sh_repo_id(const sh_repo *r, const char *path);             // the object at path ("" = the top), or ""

// Objects read through one `git cat-file --batch`, in order: begin (size -1: missing, or not a file), the content in
// pieces, end.
typedef struct {
    void (*begin)(void *user, size_t i, long long size);
    void (*data)(void *user, size_t i, const char *p, size_t n);
    void (*end)(void *user, size_t i);
    void *user;
} sh_blob_reader;
bool sh_repo_read(const sh_repo *r, const char *const *ids, size_t n, const sh_blob_reader *reader, char **err);

#define SH_SKILL_MAX_FILES 5000
#define SH_SKILL_MAX_BYTES (128LL * 1024 * 1024)

// Writes the plain files of folder `dir` ("" = the whole commit) below `dest`. Links and submodules are skipped, every path
// is checked, and one folder may hold 5000 files and 128 MB at most. All folders come out of one git process, and each
// succeeds or fails on its own (*err says why).
typedef struct {
    const char *dir;
    const char *dest;
    bool ok;
    char *err;
} sh_extract;
void sh_repo_extract(const sh_repo *r, sh_extract *jobs, size_t n);

#ifdef __cplusplus
}
#endif
