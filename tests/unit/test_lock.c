// The user lock file rules (docs/formats.md, section 2): which file is used, the merged view when both exist, and where
// changes are written. Runs in a throw-away folder with its own environment.
#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE  // setenv, unsetenv
#endif
#include "test.h"

#include <stdlib.h>

#include "sh_fs.h"
#include "sh_json.h"
#include "sh_path.h"
#include "sh_store.h"
#include "sh_str.h"

#ifdef _WIN32
static void set_env(const char *k, const char *v) { _putenv_s(k, v ? v : ""); }
#else
static void set_env(const char *k, const char *v) {
    if (v) setenv(k, v, 1);
    else unsetenv(k);
}
#endif

static char *root;

static char *at(const char *rel) { return sh_path_join(root, rel); }

static void write_text(const char *rel, const char *text) {
    char *p = at(rel);
    char *parent = sh_path_parent(p);
    sh_mkpath(parent);
    sh_write_file(p, text, strlen(text));
    free(parent);
    free(p);
}

static bool has_entry(const char *rel, const char *name) {
    char *p = at(rel);
    yyjson_doc *doc = NULL;
    bool found = false;
    if (sh_json_read(p, 0, &doc) == SH_JSON_OK) found = sh_json_obj(sh_json_obj(yyjson_doc_get_root(doc), "skills"), name) != NULL;
    yyjson_doc_free(doc);
    free(p);
    return found;
}

static void setup(void) {
    char *tmp = sh_getenv("TMPDIR");
    char *base = sh_path_clean(tmp && *tmp ? tmp : "/tmp");
    free(tmp);
    root = sh_strfmt("%s/skillhub-unit-lock-%d", base, rand());
    free(base);
    sh_remove_tree(root);
    sh_mkpath(root);
    set_env("SKILLHUB_HOME", NULL);
    char *state = at("state");
    set_env("XDG_STATE_HOME", state);
    free(state);
}

static void teardown(void) {
    sh_remove_tree(root);
    free(root);
    set_env("XDG_STATE_HOME", NULL);
}

static const char LEGACY[] = ".agents/.skill-lock.json";
static const char NPX[] = "state/skills/.skill-lock.json";

static void which_file(void) {
    setup();
    char *p, *o;
    // neither exists: create where npx looks
    sh_lock_paths_for(root, &p, &o);
    char *want = at(NPX);
    CHECK_EQ_STR(p, want);
    CHECK(o == NULL);
    free(p); free(o); free(want);
    // only the older file: keep using it
    write_text(LEGACY, "{\"version\":3,\"skills\":{}}");
    sh_lock_paths_for(root, &p, &o);
    want = at(LEGACY);
    CHECK_EQ_STR(p, want);
    CHECK(o == NULL);
    free(p); free(o); free(want);
    // both: npx's file first, the legacy one still read
    write_text(NPX, "{\"version\":3,\"skills\":{}}");
    sh_lock_paths_for(root, &p, &o);
    want = at(NPX);
    char *want_other = at(LEGACY);
    CHECK_EQ_STR(p, want);
    CHECK(o && strcmp(o, want_other) == 0);
    free(p); free(o); free(want); free(want_other);
    // SKILLHUB_HOME pins the legacy file (tests, portable setups)
    set_env("SKILLHUB_HOME", root);
    sh_lock_paths_for(root, &p, &o);
    want = at(LEGACY);
    CHECK_EQ_STR(p, want);
    CHECK(o == NULL);
    free(p); free(o); free(want);
    teardown();
}

static void both_files(void) {
    setup();
    write_text(LEGACY, "{\"version\":3,\"skills\":{"
                       "\"old\":{\"source\":\"a/b\",\"updatedAt\":\"2026-01-01T00:00:00.000Z\"},"
                       "\"shared\":{\"source\":\"from/legacy\",\"updatedAt\":\"2026-01-01T00:00:00.000Z\"}},\"dismissed\":{}}");
    write_text(NPX, "{\"version\":3,\"skills\":{"
                    "\"shared\":{\"source\":\"from/npx\",\"updatedAt\":\"2026-06-01T00:00:00.000Z\",\"pluginName\":\"kept\"}}}");
    sh_store st;
    memset(&st, 0, sizeof st);
    st.home = sh_strdup(root);
    st.central = at(".agents/skills");
    sh_lock_paths_for(root, &st.lock_path, &st.lock_other);

    // the view holds both files' entries, and the newer of two entries
    yyjson_mut_doc *view = sh_lock_load(&st, NULL);
    CHECK(view != NULL);
    yyjson_mut_val *skills = yyjson_mut_obj_get(yyjson_mut_doc_get_root(view), "skills");
    CHECK(yyjson_mut_obj_get(skills, "old") != NULL);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_obj_get(skills, "shared"), "source")), "from/npx");
    yyjson_mut_doc_free(view);

    size_t only = 0;
    CHECK(sh_lock_split(&st, &only) && only == 1);

    // a change goes to the file that holds the entry; a new entry goes to npx's file
    sh_source src = {sh_strdup("github"), sh_strdup(""), sh_strdup("x/y"), sh_strdup(""), sh_strdup("SKILL.md"), sh_strdup(""), sh_strdup("t"), sh_strdup("t")};
    CHECK(sh_lock_put(&st, "old", &src));
    CHECK(has_entry(LEGACY, "old") && !has_entry(NPX, "old"));
    CHECK(sh_lock_put(&st, "brand-new", &src));
    CHECK(has_entry(NPX, "brand-new") && !has_entry(LEGACY, "brand-new"));
    // removing takes it out of both files
    CHECK(sh_lock_put(&st, "shared", NULL));
    CHECK(!has_entry(NPX, "shared") && !has_entry(LEGACY, "shared"));
    // nothing is copied across: the legacy entry never shows up in npx's file
    CHECK(!has_entry(NPX, "old"));

    // the one-time merge: entries whose folder exists move into npx's file, the legacy file is set aside
    char *folder = at(".agents/skills/old");
    sh_mkpath(folder);
    free(folder);
    CHECK(sh_lock_migrate(&st, NULL));
    CHECK(has_entry(NPX, "old"));
    char *legacy = at(LEGACY);
    CHECK(!sh_lexists(legacy));
    CHECK(st.lock_other == NULL);
    free(legacy);
    sh_source_free(&src);
    free(st.home);
    free(st.central);
    free(st.lock_path);
    free(st.lock_other);
    teardown();
}

static void broken_file(void) {
    setup();
    set_env("SKILLHUB_HOME", root);
    write_text(LEGACY, "{\"version\":3,\"skills\":{\"a\":");  // half written
    sh_store st;
    memset(&st, 0, sizeof st);
    st.home = sh_strdup(root);
    sh_lock_paths_for(root, &st.lock_path, &st.lock_other);
    char *err = NULL;
    CHECK(sh_lock_load(&st, &err) == NULL && err != NULL);
    free(err);
    sh_source src = {sh_strdup("github"), sh_strdup(""), sh_strdup("x/y"), sh_strdup(""), sh_strdup("SKILL.md"), sh_strdup(""), sh_strdup("t"), sh_strdup("t")};
    CHECK(!sh_lock_put(&st, "b", &src));  // a file that does not parse is never overwritten
    sh_buf b = SH_BUF_INIT;
    sh_read_file(st.lock_path, 0, &b);
    CHECK_EQ_STR(sh_buf_str(&b), "{\"version\":3,\"skills\":{\"a\":");
    sh_buf_free(&b);
    write_text(LEGACY, "{\"version\":4,\"skills\":{}}");  // a newer format: left alone too
    CHECK(!sh_lock_put(&st, "b", &src));
    sh_source_free(&src);
    free(st.home);
    free(st.lock_path);
    free(st.lock_other);
    set_env("SKILLHUB_HOME", NULL);
    teardown();
}

const sh_test_case sh_tests_lock[] = {
    {"lock.which_file", which_file}, {"lock.both_files", both_files}, {"lock.broken_file", broken_file}, {NULL, NULL}};
