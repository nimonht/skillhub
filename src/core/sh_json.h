// JSON files through yyjson (third_party/yyjson). Reading keeps fields written by other tools: callers copy the parsed
// document into a mutable one, change only what they own, and write it back.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "yyjson.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SH_JSON_OK,
    SH_JSON_MISSING,    // no such file
    SH_JSON_UNREADABLE, // exists but cannot be read
    SH_JSON_INVALID,    // not JSON, or not an object
} sh_json_status;

// Reads a JSON object from `path` (at most `max` bytes, 0: no limit). On SH_JSON_OK *doc must be freed with
// yyjson_doc_free. Invalid Unicode escapes written by other programs are accepted.
sh_json_status sh_json_read(const char *path, size_t max, yyjson_doc **doc);
yyjson_doc *sh_json_parse(const char *data, size_t n);  // NULL when invalid

// Writes `doc` atomically. `two_spaces`: indentation of two spaces (as npx writes); otherwise four.
// `final_newline`: end the file with a newline.
bool sh_json_write(const char *path, yyjson_mut_doc *doc, bool two_spaces, bool final_newline);
char *sh_json_string(yyjson_mut_doc *doc, bool pretty);  // malloc'd

// Small accessors that tolerate missing keys and wrong types.
const char *sh_json_str(yyjson_val *obj, const char *key);  // "" when missing or not a string
bool sh_json_bool(yyjson_val *obj, const char *key);
long long sh_json_int(yyjson_val *obj, const char *key, long long fallback);
yyjson_val *sh_json_obj(yyjson_val *obj, const char *key);  // NULL unless an object
yyjson_val *sh_json_arr(yyjson_val *obj, const char *key);  // NULL unless an array

// Mutable helpers: set `key` on `obj` to a copy of `value` (the string is copied into the document).
void sh_json_set_str(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key, const char *value);
void sh_json_set_int(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key, long long value);
void sh_json_set_bool(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key, bool value);
void sh_json_set_val(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key, yyjson_mut_val *value);
yyjson_mut_val *sh_json_get_or_add_obj(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key);

#ifdef __cplusplus
}
#endif
