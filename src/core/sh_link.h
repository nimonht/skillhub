// Directory links: symlinks everywhere. On Windows a symlink needs Developer Mode or an administrator, so a junction
// (absolute target, no privilege needed) is made when a symlink is refused. Ported from 1.x src/platform.cpp.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool sh_is_link(const char *path);                         // the entry itself is a link (never follows it)
char *sh_link_target(const char *path);                    // absolute, cleaned target; NULL when not a link
bool sh_points_to(const char *link_path, const char *dir); // a link that leads to the folder `dir`
bool sh_make_dir_link(const char *link_path, const char *target);  // target relative to the link's folder, or absolute
bool sh_remove_link(const char *link_path);                // removes the link only, never what it points to
bool sh_can_make_dir_links(void);                          // probed once on Windows
bool sh_can_make_symlinks(void);                           // a real symlink: a junction is absolute, so it cannot be committed

#ifdef __cplusplus
}
#endif
