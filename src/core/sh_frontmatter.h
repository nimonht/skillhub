// The YAML front matter of a SKILL.md: only top-level `key: value` pairs, enough for `name` and `description`.
// Tolerant of odd files on purpose: what installs today must keep reading the same.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char **keys;
    char **values;
    size_t len;
} sh_frontmatter;

// Parses `text` (UTF-8; invalid bytes are replaced first). An unknown key gives NULL from sh_fm_get.
void sh_fm_parse(const char *text, size_t n, sh_frontmatter *out);
// Reads the first 64 KB of a SKILL.md and parses it; an unreadable file gives an empty result.
void sh_fm_read(const char *skill_md, sh_frontmatter *out);
const char *sh_fm_get(const sh_frontmatter *fm, const char *key);  // NULL when absent
const char *sh_fm_value(const sh_frontmatter *fm, const char *key); // "" when absent
void sh_fm_free(sh_frontmatter *fm);

#ifdef __cplusplus
}
#endif
