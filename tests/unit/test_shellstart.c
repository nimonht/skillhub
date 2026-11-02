// Which startup files run a system summary, and the private files that keep it quiet for one start.
#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE  // mkdtemp, setenv, symlink
#endif

#include "test.h"

#include "sh_fs.h"
#include "sh_path.h"
#include "sh_shellstart.h"
#include "sh_str.h"

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>

static char *fresh_home(void) {
    char tmpl[] = "/tmp/skillhub-shellstart-XXXXXX";
    CHECK(mkdtemp(tmpl) != NULL);
    return sh_strdup(tmpl);
}

static void put(const char *home, const char *name, const char *text) {
    char *path = sh_strfmt("%s/%s", home, name);
    CHECK(sh_write_atomic(path, text, strlen(text)));
    free(path);
}

static void finds_a_plain_call(void) {
    char *home = fresh_home();
    put(home, ".zshrc", "# a comment\nfastfetch\n");
    CHECK_STR(sh_shell_fetch_tool("/bin/zsh", home), "fastfetch");
    CHECK(sh_shell_fetch_tool("/bin/bash", home) == NULL);   // another shell's file
    put(home, ".bash_profile", "if command -v neofetch >/dev/null; then neofetch --off; fi\n");
    CHECK_STR(sh_shell_fetch_tool("/usr/bin/bash", home), "neofetch");
    sh_remove_tree(home);
    free(home);
}

static void ignores_what_only_looks_like_one(void) {
    char *home = fresh_home();
    put(home, ".zshrc", "# fastfetch\n  # neofetch\nalias ff=fastfetch\nabbr nf neofetch\nexport X=~/.config/fastfetch/config.jsonc\n"
                        "myfastfetch\nfastfetch-wrapper\necho hello # pfetch\n");
    CHECK(sh_shell_fetch_tool("/bin/zsh", home) == NULL);
    sh_remove_tree(home);
    free(home);
}

static void follows_sourced_files(void) {
    char *home = fresh_home();
    put(home, "dist/config.zsh", "source ~/dist/deep.zsh\n");
    put(home, "dist/deep.zsh", "hyfetch\n");
    put(home, ".zshrc", "[[ -f ~/dist/config.zsh ]] && source ~/dist/config.zsh\n");
    CHECK_STR(sh_shell_fetch_tool("zsh", home), "hyfetch");
    put(home, "dist/deep.zsh", ". \"$HOME/dist/deeper.zsh\"\n");
    put(home, "dist/deeper.zsh", "fastfetch\n");
    CHECK(sh_shell_fetch_tool("zsh", home) == NULL);        // beyond the depth followed
    put(home, "loop.zsh", "source ~/loop.zsh\n");
    put(home, ".zshrc", "source ~/loop.zsh\nsource ${XDG_CACHE_HOME:-$HOME/.cache}/instant.zsh\n");
    CHECK(sh_shell_fetch_tool("zsh", home) == NULL);        // a loop ends, and a computed name is passed over
    sh_remove_tree(home);
    free(home);
}

static void fish_config_and_greeting(void) {
    char *home = fresh_home();
    char *keep = sh_getenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CONFIG_HOME");
    put(home, ".config/fish/conf.d/hello.fish", "function fish_greeting\n    fastfetch\nend\n");
    CHECK_STR(sh_shell_fetch_tool("/usr/bin/fish", home), "fastfetch");
    if (keep) setenv("XDG_CONFIG_HOME", keep, 1);
    free(keep);
    sh_remove_tree(home);
    free(home);
}

static void quiet_start_per_shell(void) {
    char *home = fresh_home();
    char *dir = sh_strfmt("%s/run/skillhub-shell", home);
    sh_shell_quiet q = SH_SHELL_QUIET_INIT;
    CHECK(sh_shell_quiet_start("/bin/zsh", dir, &q));
    struct stat st;
    CHECK(stat(dir, &st) == 0 && (st.st_mode & 077) == 0);
    CHECK(q.args.len == 0);
    bool zdotdir = false;
    for (size_t i = 0; i < q.env.len; ++i) zdotdir = zdotdir || sh_starts_with(q.env.items[i], "ZDOTDIR=");
    CHECK(zdotdir);
    sh_shell_quiet_free(&q);

    sh_shell_quiet b = SH_SHELL_QUIET_INIT;
    CHECK(sh_shell_quiet_start("bash", dir, &b));
    CHECK(b.args.len == 3 && strcmp(b.args.items[0], "--rcfile") == 0 && sh_is_file(b.args.items[1]) && strcmp(b.args.items[2], "-i") == 0);
    sh_shell_quiet_free(&b);

    sh_shell_quiet f = SH_SHELL_QUIET_INIT;
    CHECK(sh_shell_quiet_start("fish", dir, &f));
    char *conf = sh_strfmt("%s/fish/vendor_conf.d/skillhub-quiet.fish", dir);
    CHECK(sh_is_file(conf));
    free(conf);
    sh_shell_quiet_free(&f);

    sh_shell_quiet n = SH_SHELL_QUIET_INIT;
    CHECK(!sh_shell_quiet_start("/usr/bin/nu", dir, &n));
    CHECK(!sh_shell_can_quiet("/bin/dash"));
    sh_shell_quiet_free(&n);
    sh_remove_tree(home);
    free(home);
    free(dir);
}

static void refuses_a_folder_others_can_write(void) {
    char *home = fresh_home();
    char *dir = sh_strfmt("%s/open", home);
    CHECK(mkdir(dir, 0700) == 0 && chmod(dir, 0777) == 0);
    sh_shell_quiet q = SH_SHELL_QUIET_INIT;
    CHECK(!sh_shell_quiet_start("/bin/zsh", dir, &q));
    char *link = sh_strfmt("%s/link", home);
    CHECK(symlink(home, link) == 0);
    CHECK(!sh_shell_quiet_start("/bin/zsh", link, &q));
    sh_shell_quiet_free(&q);
    sh_remove_tree(home);
    free(home);
    free(dir);
    free(link);
}
#endif

const sh_test_case sh_tests_shellstart[] = {
#ifndef _WIN32
    {"shellstart_finds_a_plain_call", finds_a_plain_call},
    {"shellstart_ignores_what_only_looks_like_one", ignores_what_only_looks_like_one},
    {"shellstart_follows_sourced_files", follows_sourced_files},
    {"shellstart_fish_config_and_greeting", fish_config_and_greeting},
    {"shellstart_quiet_start_per_shell", quiet_start_per_shell},
    {"shellstart_refuses_a_folder_others_can_write", refuses_a_folder_others_can_write},
#endif
    {NULL, NULL},
};
