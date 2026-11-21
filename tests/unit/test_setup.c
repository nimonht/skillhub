// What a skill needs before all of it works (sh_setup): dependencies, install scripts, paths into a Claude Code plugin.
#include "sh_fs.h"
#include "sh_path.h"
#include "sh_setup.h"
#include "sh_str.h"
#include "test.h"

static const sh_setup_note *note_for(const sh_setup *s, const char *file) {
    for (size_t i = 0; i < s->len; ++i)
        if (strcmp(s->items[i].file, file) == 0) return &s->items[i];
    return NULL;
}

static void test_manifests_by_name(void) {
    sh_setup s = SH_SETUP_INIT;
    sh_setup_file(&s, "SKILL.md");
    sh_setup_file(&s, "scripts/package.json");
    sh_setup_file(&s, "requirements.txt");
    sh_setup_file(&s, "tool/pyproject.toml");
    sh_setup_file(&s, "Gemfile");
    sh_setup_file(&s, "notes/package.md");
    sh_setup_finish(&s);
    CHECK(s.len == 4);
    const sh_setup_note *n = note_for(&s, "scripts/package.json");
    CHECK(n && n->kind == SH_SETUP_DEPENDENCIES && strcmp(n->folder, "scripts") == 0 && strcmp(n->command, "npm install") == 0);
    n = note_for(&s, "requirements.txt");
    CHECK(n && strcmp(n->folder, "") == 0 && strcmp(n->command, "pip install -r requirements.txt") == 0);
    n = note_for(&s, "tool/pyproject.toml");
    CHECK(n && strcmp(n->command, "pip install .") == 0);
    CHECK(note_for(&s, "Gemfile") != NULL);
    sh_setup_free(&s);
}

// a lock file next to the manifest names the tool, whichever comes first
static void test_lock_files_choose_the_tool(void) {
    sh_setup s = SH_SETUP_INIT;
    sh_setup_file(&s, "a/pnpm-lock.yaml");
    sh_setup_file(&s, "a/package.json");
    sh_setup_file(&s, "b/package.json");
    sh_setup_file(&s, "b/yarn.lock");
    sh_setup_file(&s, "c/package.json");
    sh_setup_file(&s, "d/pyproject.toml");
    sh_setup_file(&s, "d/uv.lock");
    sh_setup_file(&s, "e/bun.lock");   // a lock file in another folder changes nothing here
    sh_setup_file(&s, "package.json");
    sh_setup_finish(&s);
    CHECK(strcmp(note_for(&s, "a/package.json")->command, "pnpm install") == 0);
    CHECK(strcmp(note_for(&s, "b/package.json")->command, "yarn install") == 0);
    CHECK(strcmp(note_for(&s, "c/package.json")->command, "npm install") == 0);
    CHECK(strcmp(note_for(&s, "d/pyproject.toml")->command, "uv sync") == 0);
    CHECK(strcmp(note_for(&s, "package.json")->command, "npm install") == 0);
    sh_setup_free(&s);
}

static void test_scripts_and_vendored(void) {
    sh_setup s = SH_SETUP_INIT;
    sh_setup_file(&s, "scripts/install.sh");
    sh_setup_file(&s, "Setup.PS1");
    sh_setup_file(&s, "setup.py");   // packaging, not an install script for people
    sh_setup_file(&s, "scripts/node_modules/left-pad/package.json");
    sh_setup_file(&s, ".venv/lib/requirements.txt");
    sh_setup_finish(&s);
    CHECK(s.len == 2);
    const sh_setup_note *n = note_for(&s, "scripts/install.sh");
    CHECK(n && n->kind == SH_SETUP_SCRIPT && n->command == NULL);
    CHECK(note_for(&s, "Setup.PS1") != NULL);
    sh_setup_free(&s);
}

static void test_plugin_root(void) {
    sh_setup s = SH_SETUP_INIT;
    const char *a = "Read `${CLAUDE_PLUGIN_ROOT}/prompts/clusters/unsafe.md` first.\n";
    const char *b = "Run \"$CLAUDE_PLUGIN_ROOT/bin/tool\" --help\n";
    const char *c = "Plugins get ${CLAUDE_PLUGIN_ROOT} from Claude Code.\n";
    const char *d = "Nothing about plugins here.\n";
    sh_setup_text(&s, "SKILL.md", a, strlen(a));
    sh_setup_text(&s, "scripts/run.sh", b, strlen(b));
    sh_setup_text(&s, "docs/plugins.md", c, strlen(c));
    sh_setup_text(&s, "notes.md", d, strlen(d));
    sh_setup_finish(&s);
    CHECK(s.len == 3);
    const sh_setup_note *n = note_for(&s, "SKILL.md");
    CHECK(n && n->kind == SH_SETUP_PLUGIN_ROOT && strcmp(n->detail, "prompts/clusters/unsafe.md") == 0);
    CHECK(strcmp(note_for(&s, "scripts/run.sh")->detail, "bin/tool") == 0);
    CHECK(strcmp(note_for(&s, "docs/plugins.md")->detail, "") == 0);   // named, without a path
    sh_setup_free(&s);
}

// the path shown is never more than plain path characters, whatever follows the variable
static void test_plugin_root_detail_is_plain(void) {
    sh_setup s = SH_SETUP_INIT;
    const char *t = "${CLAUDE_PLUGIN_ROOT}/a\x1b[31mred; rm -rf ~\n";
    sh_setup_text(&s, "SKILL.md", t, strlen(t));
    sh_setup_finish(&s);
    CHECK(s.len == 1 && strcmp(s.items[0].detail, "a") == 0);
    sh_setup_free(&s);
}

static char *tmp_dir(void) {
#ifdef _WIN32
    char *tmp = sh_getenv("TEMP");
#else
    char *tmp = sh_getenv("TMPDIR");
#endif
    char *base = sh_path_clean(tmp && *tmp ? tmp : "/tmp");
    free(tmp);
    char *dir = sh_strfmt("%s/skillhub-unit-setup-%d", base, rand());
    free(base);
    sh_remove_tree(dir);
    sh_mkpath(dir);
    return dir;
}

static void put(const char *dir, const char *rel, const char *text) {
    char *full = sh_path_join(dir, rel), *parent = sh_path_parent(full);
    sh_mkpath(parent);
    sh_write_file(full, text, strlen(text));
    free(parent);
    free(full);
}

// on disk: a manifest that lists nothing is no note, and installed packages are not the skill's
static void test_dir(void) {
    char *dir = tmp_dir();
    put(dir, "SKILL.md", "---\nname: x\n---\nUse ${CLAUDE_PLUGIN_ROOT}/vendor/tool.md\n");
    put(dir, "scripts/package.json", "{\"name\": \"x\", \"dependencies\": {\"left-pad\": \"1\"}}");
    put(dir, "scripts/pnpm-lock.yaml", "lockfileVersion: 9\n");
    put(dir, "meta/package.json", "{\"name\": \"only-metadata\"}");
    put(dir, "requirements.txt", "# nothing yet\n\n");
    put(dir, "py/requirements.txt", "requests==2.32\n");
    put(dir, "scripts/node_modules/left-pad/package.json", "{\"dependencies\": {}}");
    put(dir, "install.sh", "#!/bin/sh\necho hi\n");
    sh_setup s;
    sh_setup_dir(dir, &s);
    CHECK(s.len == 4);
    CHECK(note_for(&s, "scripts/package.json") && strcmp(note_for(&s, "scripts/package.json")->command, "pnpm install") == 0);
    CHECK(note_for(&s, "py/requirements.txt") != NULL);
    CHECK(note_for(&s, "meta/package.json") == NULL);
    CHECK(note_for(&s, "requirements.txt") == NULL);
    CHECK(note_for(&s, "install.sh") && note_for(&s, "install.sh")->kind == SH_SETUP_SCRIPT);
    CHECK(note_for(&s, "SKILL.md") && strcmp(note_for(&s, "SKILL.md")->detail, "vendor/tool.md") == 0);
    CHECK(s.len > 0 && s.items[0].kind == SH_SETUP_DEPENDENCIES);   // dependencies first, then scripts, then plugin paths
    sh_setup_free(&s);
    sh_remove_tree(dir);
    free(dir);
}

const sh_test_case sh_tests_setup[] = {
    {"setup.manifests_by_name", test_manifests_by_name},
    {"setup.lock_files_choose_the_tool", test_lock_files_choose_the_tool},
    {"setup.scripts_and_vendored", test_scripts_and_vendored},
    {"setup.plugin_root", test_plugin_root},
    {"setup.plugin_root_detail_is_plain", test_plugin_root_detail_is_plain},
    {"setup.dir", test_dir},
    {NULL, NULL},
};
