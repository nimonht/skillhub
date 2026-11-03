// The offline check of a skill: fixed rules over its text files, no network and no judgement by a model, so the text of a skill
// cannot talk it out of a result. It finds things worth reading before an agent follows the skill; it never says a skill is safe.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "yyjson.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SH_FIND_MEDIUM = 1, SH_FIND_HIGH = 2 } sh_find_level;

typedef struct {
    const char *rule;    // "pipe-to-shell", ... (static)
    const char *title;   // the same for people (static)
    sh_find_level level;
    char *file;          // inside the skill, with '/'
    int line;            // from 1
    char *excerpt;       // printable: no control characters, invisible ones shown as <U+202E>; at most about 120 characters
} sh_finding;

typedef struct {
    sh_finding *items;
    size_t len, cap;
    bool truncated;  // the limit of findings was reached
} sh_findings;

void sh_scan_dir(const char *dir, sh_findings *out);  // every text file below `dir`; links, .git and files over 1 MB are skipped
void sh_scan_text(const char *rel, const char *text, size_t n, sh_findings *out);  // one file's content
// What `new_dir` has that `old_dir` lacks, compared by rule, file and excerpt, so a line that only moved is not new.
void sh_scan_added(const char *old_dir, const char *new_dir, sh_findings *out);
bool sh_scan_adds_high(const char *installed, const char *stage);  // the update brings a high finding
size_t sh_findings_count(const sh_findings *f, sh_find_level level);
void sh_findings_free(sh_findings *f);
void sh_findings_json(yyjson_mut_doc *d, yyjson_mut_val *list, const sh_findings *f);

#ifdef __cplusplus
}
#endif
