// Paths, always with '/' inside the core (Windows separators are converted at the boundary), and the folders Skill Hub
// uses on each system (docs/formats.md, section 1). All returned strings are malloc'd.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// QDir::cleanPath: '/' separators (backslashes converted on Windows), no empty or "." segments, ".." resolved as far as
// possible, no trailing '/'. "" stays "".
char *sh_path_clean(const char *path);
char *sh_path_join(const char *dir, const char *name);   // dir + "/" + name, no cleaning
char *sh_path_join3(const char *a, const char *b, const char *c);
char *sh_path_parent(const char *path);                  // everything before the last '/', "" if none ("/" for "/x")
const char *sh_path_name(const char *path);              // after the last '/'
// one path segment Windows cannot keep as written: a device name (CON, COM1), a stream (":"), or trailing dots or spaces
bool sh_path_windows_unsafe(const char *segment);
bool sh_path_is_absolute(const char *path);
bool sh_path_is_root(const char *path);                  // "/", "C:/", "//server/share"
// QDir(dir).relativeFilePath(path) for two absolute paths: "../../x" style, or `path` itself when there is no common root.
char *sh_path_relative(const char *dir, const char *path);
// `path` equals `dir` or lies below it (case-insensitive on Windows).
bool sh_path_within(const char *path, const char *dir);
int sh_path_cmp(const char *a, const char *b);           // strcmp, case-insensitive on Windows
bool sh_path_eq(const char *a, const char *b);

char *sh_home(void);          // $SKILLHUB_HOME, else the system's home folder
char *sh_config_dir(void);    // <config>/skillhub, or $SKILLHUB_CONFIG
char *sh_cache_dir(void);     // <cache>/skillhub, or $SKILLHUB_CACHE
char *sh_state_dir(void);     // <state>/skillhub, or $SKILLHUB_STATE
char *sh_tilde(const char *path);  // home replaced by "~" for display

// Environment variable as UTF-8, or NULL when unset. The returned string is malloc'd.
char *sh_getenv(const char *name);
bool sh_env_is_set(const char *name);  // set, even to ""

#ifdef __cplusplus
}
#endif
