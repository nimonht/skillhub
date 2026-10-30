// `skillhub fetch`: a banner of the machine and its skills, in the fashion of fastfetch. Reads what is on disk and never touches
// the network, so it can open every terminal session.
#pragma once

#include <stdbool.h>

#include "sh_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SH_COLOR_NONE, SH_COLOR_256, SH_COLOR_TRUE } sh_fetch_color;
typedef enum { SH_ICONS_UNICODE, SH_ICONS_NERD, SH_ICONS_NONE } sh_fetch_icons;

typedef struct {
    sh_fetch_color color;
    sh_fetch_icons icons;
    const char *logo;   // "builtin", "none", or the path of an image
    bool kitty_image;   // this terminal shows images: draw the logo crisply
    int cols;           // 0: no limit
} sh_fetch_opts;

char *sh_fetch_render(const sh_store *st, const sh_fetch_opts *o);  // the banner, ending in a newline
char *sh_fetch_json(const sh_store *st);
char *sh_fetch_field(const sh_store *st, const char *name);         // NULL when there is no such field

#ifdef __cplusplus
}
#endif
