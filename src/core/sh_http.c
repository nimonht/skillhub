// See sh_http.h. Moved here unchanged from sh_selfupdate.c: installed copies download their updates with exactly these arguments.
#include "sh_http.h"

#include <stdlib.h>
#include <string.h>

#include "sh_fs.h"
#include "sh_path.h"
#include "sh_proc.h"
#include "sh_str.h"
#include "sh_utf.h"

// The system's own program by its full path where the system has one, so nothing earlier on PATH can stand in for it.
static char *fetch_program(bool *wget) {
    *wget = false;
#if defined(_WIN32)
    char *root = sh_getenv("SystemRoot");
    char *curl = sh_strfmt("%s/System32/curl.exe", root && *root ? root : "C:/Windows");
    free(root);
    if (sh_is_file(curl)) return curl;
    free(curl);
    return NULL;
#elif defined(__APPLE__)
    return sh_is_file("/usr/bin/curl") ? sh_strdup("/usr/bin/curl") : NULL;
#else
    char *found = sh_find_executable("curl", NULL);
    if (found) return found;
    found = sh_find_executable("wget", NULL);
    *wget = found != NULL;
    return found;
#endif
}

bool sh_http_available(void) {
    bool wget;
    char *program = fetch_program(&wget);
    const bool ok = program != NULL;
    free(program);
    return ok;
}

bool sh_http_get(const char *url, const char *dest, int timeout_ms, char **err) {
    const bool tls = sh_starts_with(url, "https://");
    if (!tls && !sh_starts_with(url, "http://127.0.0.1:")) {  // plain http exists for the tests' server on this machine, nothing else
        *err = sh_strfmt("refusing to download from %s", url);
        return false;
    }
    bool wget;
    char *program = fetch_program(&wget);
    if (!program) {
        *err = sh_strdup("neither curl nor wget was found");
        return false;
    }
    char *output = sh_strfmt("--output-document=%s", dest);
    const char *const with_curl[] = {program, "-q", "--fail", "--location", "--silent", "--show-error", "--proto", tls ? "=https" : "=http",
                                     "--proto-redir", tls ? "=https" : "=http", "--connect-timeout", "20", "--output", dest, url, NULL};
    const char *const with_wget[] = {program, "--quiet", "--timeout=20", "--tries=2", output, url, NULL};
    const char *const with_wget_tls[] = {program, "--quiet", "--https-only", "--timeout=20", "--tries=2", output, url, NULL};
    const char *const *argv = !wget ? with_curl : tls ? with_wget_tls : with_wget;
    sh_proc_opts o = {0};
    o.timeout_ms = timeout_ms;
    sh_proc_result r = {0};
    const bool ok = sh_proc_run(argv, &o, &r);
    if (!ok) {
        char *why = sh_trimmed(sh_buf_str(&r.err), r.err.len);
        *err = r.timed_out ? sh_strfmt("the download of %s timed out", sh_path_name(dest))
             : *why        ? sh_strfmt("could not download %s: %s", sh_path_name(dest), why)
                           : sh_strfmt("could not download %s", sh_path_name(dest));
        free(why);
        sh_remove_file(dest);
    }
    sh_proc_result_free(&r);
    free(output);
    free(program);
    return ok;
}
