// What this program was built as, for sh_selfupdate.c. It is its own file, compiled per program, so that the test program
// (skillhub-update-test, never shipped) can trust a test key while the core stays the same.
#include "sh_selfupdate.h"

#ifndef SKILLHUB_UPDATE_VERSION
#define SKILLHUB_UPDATE_VERSION SKILLHUB_VERSION
#endif

void sh_update_build_info(sh_update_build *b) {
    b->version = SKILLHUB_UPDATE_VERSION;
    b->public_key = SKILLHUB_UPDATE_KEY;
    b->release_build = SKILLHUB_SELF_UPDATE != 0;
}
