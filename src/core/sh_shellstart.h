// What a shell shows as it starts: whether its startup files run a system summary of their own (fastfetch and the like), and
// how to start it so that this once it does not, without touching those files.
#pragma once

#include "sh_str.h"

#ifdef __cplusplus
extern "C" {
#endif

// The first such tool the startup files of `shell` run, malloc'd; NULL when they run none. Comments and aliases do not count, and
// files they source are followed. Reading is all this does: a call inside an `if` counts even when the condition is false.
char *sh_shell_fetch_tool(const char *shell, const char *home);

typedef struct {
    sh_strv env;   // NAME=value, for the shell's environment
    sh_strv args;  // when not empty: the shell's arguments, in place of the usual ones
} sh_shell_quiet;
#define SH_SHELL_QUIET_INIT {SH_STRV_INIT, SH_STRV_INIT}

bool sh_shell_can_quiet(const char *shell);  // zsh, bash and fish
// Writes startup files into `dir`, a private folder, that hide the tools until the first prompt (fish: the first command), read
// the user's own files as usual, and leave nothing behind in the shell. False when the shell has no such way, or `dir` is not ours.
bool sh_shell_quiet_start(const char *shell, const char *dir, sh_shell_quiet *out);
void sh_shell_quiet_free(sh_shell_quiet *q);

#ifdef __cplusplus
}
#endif
