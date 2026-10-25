// libFuzzer target: export files are untrusted (docs/formats.md, section 9). Parsing must never crash, and no item that
// passes validation may carry an unsafe name or path.
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sh_store.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    sh_store st;
    memset(&st, 0, sizeof st);
    st.central = "/nonexistent/skillhub-fuzz/skills";  // existence checks only; nothing is written
    sh_import_items items;
    char *err = NULL;
    sh_store_read_export_data(&st, (const char *)data, size, &items, &err);
    for (size_t i = 0; i < items.len; ++i) {
        const sh_import_item *it = &items.items[i];
        if (*it->problem) continue;
        assert(!strstr(it->name, "..") && !strchr(it->name, '/') && !strchr(it->name, '\\') && it->name[0] != '.');
        for (size_t f = 0; f < it->n_files; ++f) {
            const char *p = it->files[f].path;
            assert(p[0] != '/' && !strstr(p, "../") && strcmp(p, "..") != 0 && !strchr(p, '\\') && !strchr(p, ':'));
        }
    }
    sh_import_items_free(&items);
    free(err);
    return 0;
}
