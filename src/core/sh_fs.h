// Files and folders. Links are never followed when removing, copying or walking, so nothing a link points to can be
// deleted or duplicated by accident. Paths are UTF-8 with '/'; on Windows they get the \\?\ prefix at the boundary.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sh_str.h"

#ifdef __cplusplus
extern "C" {
#endif

bool sh_read_file(const char *path, size_t max, sh_buf *out);  // at most `max` bytes (0: no limit)
bool sh_write_file(const char *path, const void *data, size_t n);
// Temporary file in the same folder, flushed to disk, then renamed over `path`. A symlinked `path` is resolved first, so
// the link survives. Parent folders are created.
bool sh_write_atomic(const char *path, const void *data, size_t n);
bool sh_mkpath(const char *path);

bool sh_exists(const char *path);    // follows links: false for a dangling one
bool sh_lexists(const char *path);   // the entry itself, link or not
bool sh_is_dir(const char *path);    // follows links
bool sh_is_file(const char *path);   // follows links
bool sh_is_executable(const char *path);
int64_t sh_file_size(const char *path);
int64_t sh_mtime_ms(const char *path);  // modification time, no link following; -1 on error
char *sh_canonical(const char *path);   // links resolved, NULL when it does not exist

enum {
    SH_LIST_DIRS = 1,          // directories (and links to directories, unless SH_LIST_NO_SYMLINKS)
    SH_LIST_ALL = 2,           // every entry, dangling links included
    SH_LIST_NO_SYMLINKS = 4,   // leave links out (Windows: any reparse point)
    SH_LIST_NO_HIDDEN = 8,     // leave out names starting with '.'
    SH_SORT_NAME = 16,         // byte order
    SH_SORT_NAME_CI = 32,      // case-insensitive (sh_casecmp)
};
bool sh_list_dir(const char *path, int flags, sh_strv *names);

// Every file below `root` (hidden ones too), relative paths with '/'. Links to files are included when they lead to a
// regular file; links to folders are listed but never entered. The callback returns false to stop.
typedef bool (*sh_walk_fn)(const char *rel, const char *full, bool is_link, void *user);
bool sh_walk_files(const char *root, sh_walk_fn fn, void *user);

bool sh_remove_file(const char *path);
bool sh_remove_tree(const char *path);   // a link is removed, never what it points to
bool sh_rename(const char *from, const char *to);  // fails when `to` exists
// Rename; across volumes, copy then remove. Never moves through a link.
bool sh_move_dir(const char *from, const char *to);
bool sh_copy_file(const char *from, const char *to);  // keeps the permission bits
bool sh_copy_tree(const char *from, const char *to);  // links are skipped
bool sh_set_executable(const char *path, bool exec);  // rw-r--r-- or rwxr-xr-x
int64_t sh_newest_mtime_ms(const char *dir);          // newest modification time of the folder and anything below it

// A file written in pieces, as git streams a skill folder: created or truncated; false from sh_file_close when any write
// failed.
typedef struct sh_file sh_file;
sh_file *sh_file_create(const char *path);
bool sh_file_write(sh_file *f, const void *data, size_t n);
bool sh_file_close(sh_file *f);

// An exclusive lock on the file `path` (created when missing), held against other processes and other threads alike.
// timeout_ms 0: try once; -1: wait as long as it takes. NULL when it was not free in time. Only lock files are locked this
// way, never data files: Windows locks are mandatory and would block other tools.
typedef struct sh_flock sh_flock;
sh_flock *sh_flock_take(const char *path, int timeout_ms);
void sh_flock_release(sh_flock *l);

#ifdef _WIN32
#include <wchar.h>
wchar_t *sh_wide(const char *utf8);       // plain conversion
wchar_t *sh_wide_path(const char *path);  // with backslashes and, for absolute paths, the \\?\ prefix
char *sh_narrow(const wchar_t *w);
#endif

#ifdef __cplusplus
}
#endif
