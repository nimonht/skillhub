#include "sh_state.h"

#include <stdlib.h>

#include "sh_json.h"
#include "sh_path.h"
#include "sh_str.h"

static char *state_path(void) {
    char *config = sh_config_dir();
    char *p = sh_path_join(config, "state.json");
    free(config);
    return p;
}

yyjson_mut_doc *sh_state_load(char **err) {
    char *path = state_path();
    yyjson_doc *doc = NULL;
    const sh_json_status s = sh_json_read(path, 0, &doc);
    free(path);
    if (s != SH_JSON_OK && s != SH_JSON_MISSING) {
        if (err) *err = sh_strdup("state.json cannot be read");
        return NULL;
    }
    yyjson_mut_doc *m = doc ? yyjson_doc_mut_copy(doc, NULL) : yyjson_mut_doc_new(NULL);
    yyjson_doc_free(doc);
    if (!yyjson_mut_is_obj(yyjson_mut_doc_get_root(m))) yyjson_mut_doc_set_root(m, yyjson_mut_obj(m));
    yyjson_mut_val *root = yyjson_mut_doc_get_root(m);
    if (!yyjson_mut_obj_get(root, "version")) sh_json_set_int(m, root, "version", 1);
    return m;
}

bool sh_state_save(yyjson_mut_doc *m) {
    char *path = state_path();
    const bool ok = sh_json_write(path, m, true, true);
    free(path);
    return ok;
}

long long sh_setting_int(const char *section, const char *key, long long fallback) {
    char *config = sh_config_dir();
    char *path = sh_path_join(config, "settings.json");
    free(config);
    yyjson_doc *doc = NULL;
    long long value = fallback;
    if (sh_json_read(path, 0, &doc) == SH_JSON_OK) {
        yyjson_val *part = yyjson_obj_get(yyjson_doc_get_root(doc), section);
        yyjson_val *v = part ? yyjson_obj_get(part, key) : NULL;
        if (yyjson_is_num(v)) value = (long long)yyjson_get_num(v);
        yyjson_doc_free(doc);
    }
    free(path);
    return value;
}
