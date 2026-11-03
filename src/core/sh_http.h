// Downloads through the system's own curl or wget, so no HTTP or TLS library is linked in. Used by the updater (sh_selfupdate.c)
// and the security reports (sh_reports.c). Only https, and plain http to 127.0.0.1 for the tests' servers.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool sh_http_available(void);  // curl or wget was found
// Saves `url` to `dest`; on failure `dest` is gone and `err` says why.
bool sh_http_get(const char *url, const char *dest, int timeout_ms, char **err);

#ifdef __cplusplus
}
#endif
