// What a skill needs beyond its own files before all of it works. Skill Hub never runs what a repository ships, so it says
// what to run and where, and the person decides:
//   - dependencies a manifest lists (package.json, requirements.txt, ...), with the command its lock file asks for;
//   - an install script the author wrote, to be read before anyone runs it;
//   - paths into the Claude Code plugin the skill came from (${CLAUDE_PLUGIN_ROOT}/...), which exist only inside that plugin.
// Commands come from a fixed table here, never from the skill's text, so a skill cannot choose what a person is offered to run.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "yyjson.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SH_SETUP_DEPENDENCIES, SH_SETUP_SCRIPT, SH_SETUP_PLUGIN_ROOT } sh_setup_kind;

typedef struct {
    sh_setup_kind kind;
    char *file;           // inside the skill, with '/': the manifest, the script, or the file that names the plugin's path
    char *folder;         // the folder of `file` inside the skill, "" for the skill's own
    const char *command;  // dependencies: what to run in `folder` ("npm install", ...); NULL for the other kinds (static)
    char *detail;         // plugin root: the path it names after the variable ("prompts/x.md"), or ""; "" for the other kinds
} sh_setup_note;

typedef struct {
    sh_setup_note *items;
    size_t len, cap;
    struct sh_setup_hint *hints;  // lock files seen, applied by sh_setup_finish
    size_t n_hints, cap_hints;
} sh_setup;

#define SH_SETUP_INIT {NULL, 0, 0, NULL, 0, 0}

void sh_setup_file(sh_setup *s, const char *rel);  // by name: a manifest, a lock file, an install script
void sh_setup_text(sh_setup *s, const char *rel, const char *text, size_t n);  // by content: ${CLAUDE_PLUGIN_ROOT}/...
void sh_setup_plugin_root(sh_setup *s, const char *rel, const char *detail);   // what sh_setup_text found, found elsewhere
void sh_setup_finish(sh_setup *s);  // once every file is in: the package manager each lock file names, and a stable order
// Everything for a folder on disk: links and .git are skipped, node_modules and virtual environments are neither read nor
// reported, files over 1 MB are not read, and a manifest that lists nothing to install is no note.
void sh_setup_dir(const char *dir, sh_setup *out);
void sh_setup_free(sh_setup *s);
const char *sh_setup_kind_name(sh_setup_kind k);  // "dependencies", "script", "pluginRoot"
void sh_setup_json(yyjson_mut_doc *d, yyjson_mut_val *list, const sh_setup *s);  // [{kind, file, folder, command?, detail?}]

#ifdef __cplusplus
}
#endif
