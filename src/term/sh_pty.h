// A shell on a pseudo terminal: POSIX ptys, ConPTY on Windows. Linked into the window only.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sh_pty sh_pty;

// `program` is a full path, argv[0] included in `argv`. `env` holds NAME=value strings added to this process's environment, and
// is NULL-terminated. NULL with *err (malloc'd) when it cannot start.
sh_pty *sh_pty_spawn(const char *program, const char *const *argv, const char *cwd, const char *const *env, int cols, int rows, char **err);
long sh_pty_read(sh_pty *p, char *buf, size_t n);          // blocks; 0 when the shell has gone
long sh_pty_write(sh_pty *p, const char *buf, size_t n);
void sh_pty_resize(sh_pty *p, int cols, int rows);
// Closing a terminal ends the programs in it, not only the shell, and leaves alone what deliberately left it: on POSIX whatever
// called setsid, on Windows programs with a window of their own.
void sh_pty_hangup(sh_pty *p);                             // the shell and its programs are asked to go; a blocked read returns
void sh_pty_kill(sh_pty *p);                               // for those that do not go: ends the shell and every program it started, now
void sh_pty_free(sh_pty *p);                               // after the read has returned

#ifdef __cplusplus
}
#endif
