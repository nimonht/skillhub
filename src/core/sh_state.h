// state.json: durable state that Skill Hub owns and that is no preference (docs/formats.md, section 6). Only changed while
// holding the mutation lock.
#pragma once

#include <stdbool.h>

#include "yyjson.h"

#ifdef __cplusplus
extern "C" {
#endif

// The file as a document, an empty one when there is none yet. NULL with *err when it exists and cannot be read: it is then
// never overwritten.
yyjson_mut_doc *sh_state_load(char **err);
bool sh_state_save(yyjson_mut_doc *m);

#ifdef __cplusplus
}
#endif
