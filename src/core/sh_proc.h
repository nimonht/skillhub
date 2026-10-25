// Running other programs (git, systemctl, launchctl, schtasks, notify-send) and finding them. sh_proc_run is safe to call
// from several threads at once: the parallel engine runs one git process per worker thread. Each child gets only its own
// stdin, stdout and stderr (close-on-exec pipes and posix_spawn on POSIX, an explicit handle list on Windows).
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sh_str.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *cwd;               // NULL: the current folder
    const char *const *env;        // "NAME=value" entries added to (or replacing in) the environment, NULL-terminated
    const void *input;             // written to the program's stdin, which is then closed; NULL: stdin is empty
    size_t input_len;
    int timeout_ms;                // 0: wait as long as it takes
} sh_proc_opts;

typedef struct {
    bool started;     // the program could be started at all
    bool timed_out;   // killed after timeout_ms
    int exit_code;    // -1 when it ended through a signal
    sh_buf out;
    sh_buf err;
} sh_proc_result;

// Runs `argv` (argv[0] is the program: an absolute path, or a name looked up in PATH) and collects its output.
// Returns true when it ran and exited with code 0.
bool sh_proc_run(const char *const *argv, const sh_proc_opts *opts, sh_proc_result *r);
void sh_proc_result_free(sh_proc_result *r);

// Starts `argv` without waiting for it (desktop notifications). Call it from the main thread only: it forks on POSIX.
bool sh_proc_detach(const char *const *argv);

// Like Qt's QStandardPaths::findExecutable(name, dirs): the first executable `name` in `dirs` (PATH when dirs is NULL);
// on Windows the extensions in PATHEXT are tried. NULL when not found.
char *sh_find_executable(const char *name, const sh_strv *dirs);
void sh_path_dirs(sh_strv *out);  // the folders listed in PATH

// "git", or its full path where the desktop PATH is too short to find it (1.x platform::gitProgram).
const char *sh_git_program(void);

#ifdef __cplusplus
}
#endif
