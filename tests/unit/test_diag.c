// The diagnostic report (sh_diag.h): what it says about this process, and that it keeps the home folder out of what a user sends.
#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE  // setenv
#endif
#include "test.h"

#include <stdlib.h>
#include <string.h>

#include "sh_diag.h"
#include "sh_fs.h"
#include "sh_path.h"
#include "sh_store.h"
#include "sh_str.h"

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void shorten(void) {
    CHECK_STR(sh_diag_shorten("{\"a\":\"/home/u/x\",\"b\":\"/home/u\"}", "/home/u"), "{\"a\":\"~/x\",\"b\":\"~\"}");
    CHECK_STR(sh_diag_shorten("nothing to do", "/home/u"), "nothing to do");
    CHECK_STR(sh_diag_shorten("/a/b", "/"), "/a/b");   // a root would turn every slash into ~
    CHECK_STR(sh_diag_shorten("/a/b", ""), "/a/b");
    CHECK_STR(sh_diag_shorten("/a/b", NULL), "/a/b");
    CHECK_STR(sh_diag_shorten("", "/home/u"), "");
    CHECK_STR(sh_diag_shorten("/home/u/home/u", "/home/u"), "~/home/u");   // home/u inside it is a folder of its own
    CHECK_STR(sh_diag_shorten("/home/user/x /home/u", "/home/u"), "/home/user/x ~");   // another user's folder stays as it is
#ifdef _WIN32
    // a variable or a file may spell the store's home with backslashes, or in another case: it is the same folder
    CHECK_STR(sh_diag_shorten("SKILLHUB_HOME C:\\Users\\Me\\tmp", "C:/Users/me"), "SKILLHUB_HOME ~\\tmp");
#else
    CHECK_STR(sh_diag_shorten("/HOME/U/x", "/home/u"), "/HOME/U/x");   // another folder here
#endif
}

static void memory(void) {
    sh_diag_memory m;
    CHECK(sh_diag_memory_read(&m));
    CHECK(m.rss_kb > 0);
    CHECK(m.peak_kb == -1 || m.peak_kb >= m.rss_kb / 2);
    char *text = sh_diag_memory_text(&m);
    CHECK(strstr(text, "resident") != NULL);
    free(text);
    const sh_diag_memory none = {-1, -1, -1, -1, -1};
    CHECK_STR(sh_diag_memory_text(&none), "not available on this system");
    const sh_diag_memory some = {2048, 4096, -1, 3, -1};
    CHECK_STR(sh_diag_memory_text(&some), "2.0 MB resident (peak 4.0 MB), 3 threads");
}

static void report(void) {
#ifdef _WIN32
    char *tmp = sh_getenv("TEMP");
#else
    char *tmp = sh_getenv("TMPDIR");
#endif
    char *base = sh_path_clean(tmp && *tmp ? tmp : "/tmp");
    free(tmp);
    char *root = sh_strfmt("%s/skillhub-unit-diag-%d", base, rand());
    free(base);
    sh_remove_tree(root);
    char *config = sh_path_join(root, "config"), *settings = sh_path_join(config, "settings.json");
    sh_mkpath(config);
    char *body = sh_strfmt("{\"projects\":[\"%s/work\"]}", root);
    sh_write_file(settings, body, strlen(body));
    set_env("SKILLHUB_CONFIG", config);
    set_env("SKILLHUB_AUTOMATION_DRYRUN", "systemd");   // no systemctl call from a test

    sh_store st;
    memset(&st, 0, sizeof st);
    st.home = sh_strdup(root);
    st.central = sh_path_join(root, "skills");
    st.backup = sh_path_join(root, "backup");
    st.cache = sh_path_join(root, "cache");
    st.state = sh_path_join(root, "state");
    st.lock_path = sh_path_join(root, "lock.json");
    st.parked = sh_path_join(root, "parked");
    st.scope = sh_strdup("user");
    char *text = sh_diag_report(&st, "test program");
    CHECK(strstr(text, "Skill Hub diagnostic report") != NULL);
    CHECK(strstr(text, "skillhub " SKILLHUB_VERSION ", test program, process ") != NULL);
    static const char *const parts[] = {"Machine", "Tools", "Folders", "Agents", "Skills", "Environment", "Settings (settings.json)"};
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; ++i) CHECK(strstr(text, parts[i]) != NULL);
    CHECK(strstr(text, "none managed") != NULL);
    CHECK(strstr(text, "0 skills") != NULL);
    CHECK(strstr(text, "\"projects\"") != NULL);   // the settings are in it, as they are
    CHECK(strstr(text, root) == NULL);             // and nowhere does the home folder appear as itself
    free(text);

    free(st.home);
    free(st.central);
    free(st.backup);
    free(st.cache);
    free(st.state);
    free(st.lock_path);
    free(st.parked);
    free(st.scope);
    free(body);
    free(settings);
    free(config);
    sh_remove_tree(root);
    free(root);
}

const sh_test_case sh_tests_diag[] = {
    {"diag.shorten", shorten}, {"diag.memory", memory}, {"diag.report", report}, {NULL, NULL},
};
