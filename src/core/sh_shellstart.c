#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE  // lstat
#endif

#include "sh_shellstart.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "sh_fs.h"
#include "sh_path.h"

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#define MAX_STARTUP_FILE (1024 * 1024)
#define SOURCE_DEPTH 2  // ~/.zshrc -> a distribution's config -> what that sources
#define TOOL_NAMES "fastfetch neofetch screenfetch pfetch hyfetch nitch macchina ufetch"

static const char *const TOOLS[] = {"fastfetch", "neofetch", "screenfetch", "pfetch", "hyfetch", "nitch", "macchina", "ufetch"};

typedef enum { SHELL_ZSH, SHELL_BASH, SHELL_FISH, SHELL_NU, SHELL_OTHER } shell_kind;

static shell_kind kind_of(const char *shell) {
    const char *name = strrchr(shell, '/');
    name = name ? name + 1 : shell;
    return strcmp(name, "zsh") == 0 ? SHELL_ZSH : strcmp(name, "bash") == 0 ? SHELL_BASH : strcmp(name, "fish") == 0 ? SHELL_FISH : strcmp(name, "nu") == 0 ? SHELL_NU : SHELL_OTHER;
}

static const char *skip_blanks(const char *s) {
    while (*s == ' ' || *s == '\t') ++s;
    return s;
}

static bool starts_word(char c) { return strchr(" \t;&|(/\"'`", c) != NULL; }
static bool ends_word(char c) { return c == '\0' || strchr(" \t\r;&|)\"'`", c) != NULL; }

static const char *tool_on_line(const char *line) {
    for (size_t t = 0; t < sizeof TOOLS / sizeof TOOLS[0]; ++t) {
        const size_t n = strlen(TOOLS[t]);
        for (const char *at = strstr(line, TOOLS[t]); at; at = strstr(at + 1, TOOLS[t]))
            if ((at == line || starts_word(at[-1])) && ends_word(at[n])) return TOOLS[t];
    }
    return NULL;
}

// the file of `source FILE` or `. FILE`, also after `[ -f FILE ] &&`; NULL for any other line, and for a name only a shell can work out
static char *sourced_file(const char *line, const char *home) {
    const char *and_then = strstr(line, "&&");
    const char *s = skip_blanks(and_then ? and_then + 2 : line);
    if (sh_starts_with(s, "source ")) s += 7;
    else if (sh_starts_with(s, ". ")) s += 2;
    else return NULL;
    s = skip_blanks(s);
    const char quote = *s == '"' || *s == '\'' ? *s++ : '\0';
    size_t n = 0;
    while (s[n] && (quote ? s[n] != quote : !strchr(" \t\r;&|)", s[n]))) ++n;
    char *raw = sh_strndup(s, n), *path = NULL;
    if (sh_starts_with(raw, "~/")) path = sh_strfmt("%s%s", home, raw + 1);
    else if (sh_starts_with(raw, "$HOME/")) path = sh_strfmt("%s%s", home, raw + 5);
    else if (sh_starts_with(raw, "${HOME}/")) path = sh_strfmt("%s%s", home, raw + 7);
    else if (*raw == '/') path = sh_strdup(raw);
    free(raw);
    if (path && strpbrk(path, "$`(")) {
        free(path);
        path = NULL;
    }
    return path;
}

static const char *tool_in_file(const char *path, const char *home, int depth) {
    if (!sh_is_file(path)) return NULL;
    sh_buf text = SH_BUF_INIT;
    const char *found = NULL;
    if (sh_read_file(path, MAX_STARTUP_FILE, &text)) {
        sh_strv lines = SH_STRV_INIT;
        sh_split(sh_buf_str(&text), text.len, '\n', true, &lines);
        for (size_t i = 0; i < lines.len && !found; ++i) {
            char *line = lines.items[i];
            const char *code = skip_blanks(line);
            if (*code == '#' || sh_starts_with(code, "alias ") || sh_starts_with(code, "abbr ")) continue;
            char *note = strstr(line, " #");
            if (note) *note = '\0';
            found = tool_on_line(code);
            if (!found && depth > 0) {
                char *next = sourced_file(code, home);
                if (next) found = tool_in_file(next, home, depth - 1);
                free(next);
            }
        }
        sh_strv_free(&lines);
    }
    sh_buf_free(&text);
    return found;
}

static char *config_home(const char *home) {
    char *v = sh_getenv("XDG_CONFIG_HOME");
    if (v && *v) return v;
    free(v);
    return sh_strfmt("%s/.config", home);
}

static void startup_files(shell_kind kind, const char *home, sh_strv *out) {
    char *config = config_home(home);
    if (kind == SHELL_ZSH) {
        char *z = sh_getenv("ZDOTDIR");
        const char *base = z && *z ? z : home;
        sh_strv_push(out, "/etc/zsh/zshenv");
        sh_strv_push(out, "/etc/zsh/zprofile");
        sh_strv_push(out, "/etc/zsh/zshrc");
        sh_strv_push(out, "/etc/zshrc");
        static const char *const own[] = {".zshenv", ".zprofile", ".zshrc", ".zlogin"};
        for (size_t i = 0; i < sizeof own / sizeof own[0]; ++i) sh_strv_take(out, sh_strfmt("%s/%s", base, own[i]));
        free(z);
    } else if (kind == SHELL_BASH) {
        sh_strv_push(out, "/etc/profile");
        sh_strv_push(out, "/etc/bash.bashrc");
        sh_strv_push(out, "/etc/bashrc");
        static const char *const own[] = {".bash_profile", ".bash_login", ".profile", ".bashrc"};
        for (size_t i = 0; i < sizeof own / sizeof own[0]; ++i) sh_strv_take(out, sh_strfmt("%s/%s", home, own[i]));
    } else if (kind == SHELL_FISH) {
        sh_strv_push(out, "/etc/fish/config.fish");
        sh_strv_take(out, sh_strfmt("%s/fish/config.fish", config));
        sh_strv_take(out, sh_strfmt("%s/fish/functions/fish_greeting.fish", config));
        char *conf = sh_strfmt("%s/fish/conf.d", config);
        sh_strv names = SH_STRV_INIT;
        if (sh_list_dir(conf, SH_LIST_ALL | SH_LIST_NO_HIDDEN | SH_SORT_NAME, &names))
            for (size_t i = 0; i < names.len; ++i)
                if (sh_ends_with(names.items[i], ".fish")) sh_strv_take(out, sh_strfmt("%s/%s", conf, names.items[i]));
        sh_strv_free(&names);
        free(conf);
    } else if (kind == SHELL_NU) {
        sh_strv_take(out, sh_strfmt("%s/nushell/env.nu", config));
        sh_strv_take(out, sh_strfmt("%s/nushell/config.nu", config));
    } else {
        sh_strv_take(out, sh_strfmt("%s/.profile", home));
    }
    free(config);
}

char *sh_shell_fetch_tool(const char *shell, const char *home) {
    if (!shell || !*shell || !home || !*home) return NULL;
    sh_strv files = SH_STRV_INIT;
    startup_files(kind_of(shell), home, &files);
    const char *found = NULL;
    for (size_t i = 0; i < files.len && !found; ++i) found = tool_in_file(files.items[i], home, SOURCE_DEPTH);
    sh_strv_free(&files);
    return found ? sh_strdup(found) : NULL;
}

bool sh_shell_can_quiet(const char *shell) {
    const shell_kind k = shell ? kind_of(shell) : SHELL_OTHER;
#ifdef _WIN32
    (void)k;
    return false;
#else
    return k == SHELL_ZSH || k == SHELL_BASH || k == SHELL_FISH;
#endif
}

#define WRITTEN_BY "# Written by Skill Hub for the terminal it starts. Fetch tools stay quiet this once; your own files are read as usual.\n"

// zsh reads this first, with ZDOTDIR pointing here. From then on it finds the user's files, because ZDOTDIR is theirs again.
#define ZSH_ENV WRITTEN_BY                                                                          \
    "if [[ -n ${SKILLHUB_ZDOTDIR-} ]]; then ZDOTDIR=$SKILLHUB_ZDOTDIR; else unset ZDOTDIR; fi\n"     \
    "unset SKILLHUB_ZDOTDIR\n"                                                                       \
    "[[ -r ${ZDOTDIR:-$HOME}/.zshenv ]] && source ${ZDOTDIR:-$HOME}/.zshenv\n"                       \
    "typeset -ga __skillhub_quiet=(" TOOL_NAMES ")\n"                                                \
    "for __skillhub_t in $__skillhub_quiet; do eval \"function $__skillhub_t { : }\"; done\n"        \
    "unset __skillhub_t\n"                                                                           \
    "__skillhub_loud() {\n"                                                                          \
    "  unfunction $__skillhub_quiet 2>/dev/null\n"                                                   \
    "  precmd_functions=(${precmd_functions:#__skillhub_loud})\n"                                    \
    "  unset __skillhub_quiet\n"                                                                     \
    "  unfunction __skillhub_loud\n"                                                                 \
    "}\n"                                                                                            \
    "typeset -ga precmd_functions\n"                                                                 \
    "precmd_functions+=(__skillhub_loud)\n"

// what a login bash reads, in the same order
#define BASH_RC WRITTEN_BY                                                                          \
    "for __skillhub_t in " TOOL_NAMES "; do eval \"$__skillhub_t() { :; }\"; done\n"                 \
    "unset __skillhub_t\n"                                                                           \
    "[ -r /etc/profile ] && . /etc/profile\n"                                                        \
    "if [ -r ~/.bash_profile ]; then . ~/.bash_profile\n"                                            \
    "elif [ -r ~/.bash_login ]; then . ~/.bash_login\n"                                              \
    "elif [ -r ~/.profile ]; then . ~/.profile\n"                                                    \
    "fi\n"                                                                                           \
    "unset -f " TOOL_NAMES "\n"

// read before the user's config. Until the first command, since a fish greeting runs around the first prompt.
#define FISH_CONF WRITTEN_BY                                                                        \
    "for __skillhub_t in " TOOL_NAMES "\n"                                                           \
    "    function $__skillhub_t; end\n"                                                              \
    "end\n"                                                                                          \
    "set -e __skillhub_t\n"                                                                          \
    "if test -n \"$SKILLHUB_XDG_DATA_DIRS\"\n"                                                       \
    "    set -gx XDG_DATA_DIRS $SKILLHUB_XDG_DATA_DIRS\n"                                            \
    "else\n"                                                                                         \
    "    set -e XDG_DATA_DIRS\n"                                                                     \
    "end\n"                                                                                          \
    "set -e SKILLHUB_XDG_DATA_DIRS\n"                                                                \
    "function __skillhub_loud --on-event fish_preexec\n"                                             \
    "    functions -e " TOOL_NAMES " __skillhub_loud\n"                                              \
    "end\n"

#ifndef _WIN32
// these files get run: the folder must be ours alone, as $XDG_RUNTIME_DIR is
static bool private_dir(const char *dir) {
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        char *parent = sh_path_parent(dir);
        const bool made = sh_mkpath(parent) && mkdir(dir, 0700) == 0;
        free(parent);
        if (!made) return false;
    }
    struct stat st;
    return lstat(dir, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && (st.st_mode & 022) == 0;
}

static bool write_text(const char *dir, const char *name, const char *text, char **path_out) {
    char *path = sh_strfmt("%s/%s", dir, name);
    const bool ok = sh_write_atomic(path, text, strlen(text));
    if (ok && path_out) *path_out = path;
    else free(path);
    return ok;
}
#endif

bool sh_shell_quiet_start(const char *shell, const char *dir, sh_shell_quiet *out) {
#ifdef _WIN32
    (void)dir;
    (void)out;
    (void)shell;
    return false;
#else
    if (!sh_shell_can_quiet(shell) || !dir || !*dir || !private_dir(dir)) return false;
    const shell_kind kind = kind_of(shell);
    if (kind == SHELL_ZSH) {
        char *folder = sh_strfmt("%s/zsh", dir);
        const bool ok = sh_mkpath(folder) && write_text(folder, ".zshenv", ZSH_ENV, NULL);
        if (ok) {
            char *mine = sh_getenv("ZDOTDIR");
            if (mine && *mine) sh_strv_take(&out->env, sh_strfmt("SKILLHUB_ZDOTDIR=%s", mine));
            sh_strv_take(&out->env, sh_strfmt("ZDOTDIR=%s", folder));
            free(mine);
        }
        free(folder);
        return ok;
    }
    if (kind == SHELL_BASH) {
        char *rc = NULL;
        if (!write_text(dir, "bashrc", BASH_RC, &rc)) return false;
        sh_strv_push(&out->args, "--rcfile");
        sh_strv_take(&out->args, rc);
        sh_strv_push(&out->args, "-i");
        return true;
    }
    char *folder = sh_strfmt("%s/fish/vendor_conf.d", dir);
    const bool ok = sh_mkpath(folder) && write_text(folder, "skillhub-quiet.fish", FISH_CONF, NULL);
    if (ok) {
        char *mine = sh_getenv("XDG_DATA_DIRS");
        const char *dirs = mine && *mine ? mine : "/usr/local/share:/usr/share";  // what an unset variable means
        sh_strv_take(&out->env, sh_strfmt("SKILLHUB_XDG_DATA_DIRS=%s", mine && *mine ? mine : ""));
        sh_strv_take(&out->env, sh_strfmt("XDG_DATA_DIRS=%s:%s", dir, dirs));
        free(mine);
    }
    free(folder);
    return ok;
#endif
}

void sh_shell_quiet_free(sh_shell_quiet *q) {
    sh_strv_free(&q->env);
    sh_strv_free(&q->args);
}
