// libFuzzer target: repository names typed by users and read from export files.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sh_store.h"
#include "sh_str.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *s = sh_strndup((const char *)data, size);
    char *url = NULL;
    char *slug = sh_slug_from_input(s, &url);
    if (slug) {
        // a slug is exactly owner/repo made of safe characters
        const char *slash = strchr(slug, '/');
        if (!slash || strchr(slash + 1, '/') || slash == slug || !slash[1]) abort();
        for (const char *c = slug; *c; ++c)
            if (!(*c == '/' || *c == '.' || *c == '-' || *c == '_' || (*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z'))) abort();
    }
    free(slug);
    free(url);
    free(s);
    return 0;
}
