// Where the running program lives, with links resolved. The command line looks for the window next to itself, and
// automation (systemd, launchd, Task Scheduler) must name the real program, not a link to it.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Writes the absolute UTF-8 path of the running executable, links resolved, into `buf` (NUL-terminated).
// Returns the length, or 0 when it cannot be found or does not fit.
size_t sh_exe_path(char *buf, size_t size);

// Same, but only the folder (no trailing separator). Returns the length, or 0.
size_t sh_exe_dir(char *buf, size_t size);

#ifdef __cplusplus
}
#endif
