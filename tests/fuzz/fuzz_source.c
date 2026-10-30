// libFuzzer target: sources typed by users and read from export files.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sh_origin.h"
#include "sh_str.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *s = sh_strndup((const char *)data, size);
    sh_origin o;
    if (sh_origin_parse(s, &o, NULL)) {
        // what reaches git is never an option, and the cache folder stays inside the cache
        if (*o.url == '-' || !*o.url || !*o.source || !sh_origin_ref_ok(o.ref)) abort();
        char *dir = sh_origin_cache_dir(&o, "/cache");
        if (!sh_starts_with(dir, "/cache/repos/") || strstr(dir, "/../") || sh_ends_with(dir, "/..")) abort();
        free(dir);
        sh_origin_free(&o);
    }
    free(s);
    return 0;
}
