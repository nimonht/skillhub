// libFuzzer target: lock files written by other tools (npx skills, editors, half-written files).
#include <stddef.h>
#include <stdint.h>

#include "sh_json.h"
#include "sh_store.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    yyjson_doc *doc = sh_json_parse((const char *)data, size);
    if (!doc) return 0;
    yyjson_val *skills = sh_json_obj(yyjson_doc_get_root(doc), "skills");
    size_t i, n;
    yyjson_val *key, *val;
    if (!skills) {  // yyjson_obj_foreach does pointer arithmetic on NULL before checking the size
        yyjson_doc_free(doc);
        return 0;
    }
    yyjson_obj_foreach(skills, i, n, key, val) {
        sh_source s;
        sh_source_from_json(val, &s);
        (void)sh_source_tracked(&s);
        sh_source_free(&s);
    }
    yyjson_doc_free(doc);
    return 0;
}
