// What an update would change: the installed folder against upstream's, compared by git with every user-configured diff
// helper switched off, since the files are untrusted.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *path;     // inside the skill
    char status;    // 'A', 'M' or 'D'
    int added, removed;
    bool binary;
    bool exec;      // executable after the update
    bool risk;      // a script or a program that is new or changed
    bool cut;       // the patch below stops at the limit
    char *patch;    // the hunks, from the first @@; "" for binary files and mode changes
} sh_diff_file;

typedef struct {
    char *name;
    char *stage;       // what sh_engine_apply takes; "" when nothing differs
    char *tree;        // upstream's tree id of the reviewed version
    bool available;    // upstream has a newer version; otherwise the differences are local edits
    bool local_edits;  // the installed folder is not what Skill Hub placed there
    bool risk;         // some file is a risk
    bool truncated;    // some patch was cut, or the whole review reached its limit
    sh_diff_file *files;
    size_t n;
    char *err;         // why there is no review
} sh_review;

// Fills `r` from the folders `old` and `new` inside `stage`.
void sh_review_compare(const char *stage, sh_review *r);
void sh_review_free(sh_review *r);
bool sh_is_risky_file(const char *rel, const char *full);  // a script by name or first line, or an executable
// `stage` holds a script or program that `installed` lacks, or has with other content.
bool sh_adds_scripts(const char *stage, const char *installed);

#ifdef __cplusplus
}
#endif
