// The projects Skill Hub knows about (projects.json, docs/formats.md section 6). Finding them only reads the file system:
// git never runs inside a project, whose config could name hooks or an fsmonitor.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sh_str.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *path;
    char *name;
    bool auto_update;  // the daily update covers it too
} sh_project;

typedef struct { sh_project *items; size_t len; } sh_projects;

bool sh_projects_load(sh_projects *out, char **err);  // false with *err when projects.json cannot be read
void sh_projects_free(sh_projects *p);
bool sh_projects_add(const char *path, char **err);
bool sh_projects_remove(const char *path, char **err);
bool sh_projects_set_auto(const char *path, bool on, char **err);
// Folders below the roots of settings.json (projects.scanRoots, ~/Projects by default) that hold skills and are not listed yet.
void sh_projects_suggest(sh_strv *out);

#ifdef __cplusplus
}
#endif
