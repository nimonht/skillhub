// See sh_scan.h. Each rule looks at one line at a time, lowercased for the ASCII parts. Levels are tuned on real skill collections
// (anthropics/skills, vercel-labs/agent-skills and a working library of 70 skills): a rule that fires "high" on official skills is too
// broad, so documentation that merely mentions a token stays medium.
#include "sh_scan.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sh_fs.h"
#include "sh_path.h"
#include "sh_str.h"
#include "sh_utf.h"

#define MAX_FILE (1024 * 1024)
#define MAX_FINDINGS 200
#define MAX_PER_RULE_AND_FILE 3
#define EXCERPT 120

static void add(sh_findings *f, const char *rule, const char *title, sh_find_level level, const char *rel, int line, const char *excerpt) {
    size_t same = 0;
    for (size_t i = 0; i < f->len; ++i)
        if (f->items[i].rule == rule && strcmp(f->items[i].file, rel) == 0) ++same;
    if (same >= MAX_PER_RULE_AND_FILE) return;
    if (f->len >= MAX_FINDINGS) {
        f->truncated = true;
        return;
    }
    if (f->len == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 16;
        f->items = sh_xrealloc(f->items, f->cap * sizeof *f->items);
    }
    f->items[f->len++] = (sh_finding){rule, title, level, sh_strdup(rel), line, sh_strdup(excerpt)};
}

void sh_findings_free(sh_findings *f) {
    for (size_t i = 0; i < f->len; ++i) {
        free(f->items[i].file);
        free(f->items[i].excerpt);
    }
    free(f->items);
    memset(f, 0, sizeof *f);
}

size_t sh_findings_count(const sh_findings *f, sh_find_level level) {
    size_t n = 0;
    for (size_t i = 0; i < f->len; ++i) n += f->items[i].level == level;
    return n;
}

void sh_findings_json(yyjson_mut_doc *d, yyjson_mut_val *list, const sh_findings *f) {
    for (size_t i = 0; i < f->len; ++i) {
        const sh_finding *x = &f->items[i];
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, o, "rule", x->rule);
        yyjson_mut_obj_add_str(d, o, "title", x->title);
        yyjson_mut_obj_add_str(d, o, "level", x->level == SH_FIND_HIGH ? "high" : "medium");
        yyjson_mut_obj_add_strcpy(d, o, "file", x->file);
        yyjson_mut_obj_add_int(d, o, "line", x->line);
        yyjson_mut_obj_add_strcpy(d, o, "excerpt", x->excerpt);
        yyjson_mut_arr_append(list, o);
    }
}

// ---- characters ----

static bool bidi_control(uint32_t c) { return (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069); }
static bool tag_character(uint32_t c) { return c >= 0xE0000 && c <= 0xE007F; }
static bool zero_width(uint32_t c) { return c == 0x200B || c == 0x200C || c == 0x200D || (c >= 0x2060 && c <= 0x2064) || c == 0xFEFF; }
static bool pictograph(uint32_t c) {  // what a zero-width joiner legitimately sits between: emoji and their modifiers
    return (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) || (c >= 0x2B00 && c <= 0x2BFF) || c == 0xFE0F || c == 0x20E3;
}
static bool shown_as_code(uint32_t c) { return bidi_control(c) || tag_character(c) || zero_width(c) || c == 0x200E || c == 0x200F; }

// The line around byte `at`, as people can read it: control characters gone (sh_printable), invisible ones written out.
static char *excerpt_of(const char *line, size_t n, size_t at) {
    size_t from = at > 40 ? at - 40 : 0;
    while (from > 0 && ((unsigned char)line[from] & 0xC0) == 0x80) --from;  // not in the middle of a character
    sh_buf b = SH_BUF_INIT;
    const char *p = line + from, *end = line + n;
    size_t shown = 0;
    while (p < end && shown < EXCERPT) {
        const char *start = p;
        const uint32_t c = sh_utf8_next(&p, end);
        if (shown_as_code(c)) sh_buf_printf(&b, "<U+%04X>", (unsigned)c);
        else sh_buf_append(&b, start, (size_t)(p - start));
        ++shown;
    }
    if (p < end) sh_buf_puts(&b, " ...");
    char *raw = sh_buf_steal(&b), *clean = sh_printable(raw ? raw : "", false);
    free(raw);
    char *trimmed = sh_trimmed(clean, strlen(clean));
    free(clean);
    return trimmed;
}

static void report(sh_findings *f, const char *rule, const char *title, sh_find_level level, const char *rel, int line_no, const char *line,
                   size_t n, size_t at) {
    char *e = excerpt_of(line, n, at);
    add(f, rule, title, level, rel, line_no, e);
    free(e);
}

// ---- words ----

// `lower` (ASCII-lowercased) holds `word` at `at`, not inside a longer name
static bool word_at(const char *lower, size_t n, size_t at, const char *word) {
    const size_t w = strlen(word);
    if (at + w > n || memcmp(lower + at, word, w) != 0) return false;
    const bool before = at == 0 || !(sh_uni_is_alnum((unsigned char)lower[at - 1]) || lower[at - 1] == '_' || lower[at - 1] == '-');
    const bool after = at + w == n || !(sh_uni_is_alnum((unsigned char)lower[at + w]) || lower[at + w] == '_' || lower[at + w] == '-');
    return before && after;
}

static const char *find_word(const char *lower, size_t n, const char *word, size_t from) {
    for (const char *p = lower + from; (p = strstr(p, word)) != NULL; ++p)
        if (word_at(lower, n, (size_t)(p - lower), word)) return p;
    return NULL;
}

static size_t skip_spaces(const char *s, size_t n, size_t i) {
    while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
    return i;
}

// ---- rules ----

static const char *const FETCHERS[] = {"curl", "wget", "iwr", "irm", "invoke-webrequest", "invoke-restmethod", "fetch", NULL};
static const char *const SHELLS[] = {"sh", "bash", "zsh", "dash", "ksh", "fish", "python", "python3", "perl", "ruby", "node", "iex", "invoke-expression",
                                     "pwsh", "powershell", NULL};

static bool in_list(const char *const *list, const char *s, size_t n) {
    for (; *list; ++list)
        if (strlen(*list) == n && memcmp(*list, s, n) == 0) return true;
    return false;
}

// `curl ... | sh`, `wget -O- ... | sudo bash`, `bash <(curl ...)`, `eval "$(curl ...)"`, `iex (iwr ...)`
static long pipe_to_shell(const char *lower, size_t n) {
    for (const char *const *f = FETCHERS; *f; ++f) {
        const char *hit = find_word(lower, n, *f, 0);
        if (!hit) continue;
        const size_t at = (size_t)(hit - lower), after = at + strlen(*f);
        if (after >= n || (lower[after] != ' ' && lower[after] != '\t')) continue;  // a command with arguments, not a word in a regex
        const size_t arg = skip_spaces(lower, n, after);
        if (arg >= n || lower[arg] == '|') continue;  // "curl | sh" with nothing to fetch is a mention, such as a warning against it
        for (size_t i = after; i < n; ++i) {
            if (lower[i] != '|' || (i + 1 < n && lower[i + 1] == '|')) continue;
            size_t j = skip_spaces(lower, n, i + 1);
            if (n - j >= 5 && memcmp(lower + j, "sudo ", 5) == 0) j = skip_spaces(lower, n, j + 5);
            if (n - j >= 3 && memcmp(lower + j, "-e ", 3) == 0) j = skip_spaces(lower, n, j + 3);
            size_t k = j;
            while (k < n && (sh_uni_is_alnum((unsigned char)lower[k]) || lower[k] == '-')) ++k;
            if (k > j && in_list(SHELLS, lower + j, k - j)) return (long)at;
        }
        // the shell reads what the fetcher prints: <( curl ...), $( curl ...), (iwr ...) handed to iex
        for (size_t i = at; i > 0; --i) {
            const char c = lower[i - 1];
            if (c == ' ' || c == '\t' || c == '"') continue;
            if (c == '(' && i >= 2 && (lower[i - 2] == '<' || lower[i - 2] == '$')) {
                if (lower[i - 2] == '<') return (long)at;
                const char *e = strstr(lower, "eval");
                if (e && e < hit) return (long)at;
            }
            if (c == '(' && strstr(lower, "iex") && strstr(lower, "iex") < hit) return (long)at;
            break;
        }
    }
    return -1;
}

static bool dangerous_target(const char *t, size_t n) {
    static const char *const TARGETS[] = {"/", "/*", "~", "~/", "~/*", "$home", "$home/", "${home}", "\"$home\"", "/home", "/etc", "/usr", NULL};
    while (n && (t[n - 1] == ';' || t[n - 1] == '\'')) --n;
    if (n && t[0] == '\'') ++t, --n;
    return in_list(TARGETS, t, n);
}

// `rm -rf /`, `rm -fr ~`, `rm -r -f "$HOME"`, `mkfs.ext4 /dev/sda`, `dd if=... of=/dev/sda`, `chmod -R 777 /`, a fork bomb
static long destructive(const char *lower, size_t n) {
    for (const char *p = lower; (p = find_word(lower, n, "rm", (size_t)(p - lower))) != NULL; p += 2) {
        size_t i = (size_t)(p - lower) + 2;
        bool recursive = false;
        for (;;) {
            i = skip_spaces(lower, n, i);
            size_t k = i;
            while (k < n && lower[k] != ' ' && lower[k] != '\t') ++k;
            if (k == i) break;
            if (lower[i] == '-') {
                recursive = recursive || memchr(lower + i, 'r', k - i) || (k - i >= 11 && memcmp(lower + i, "--recursive", 11) == 0);
                i = k;
                continue;
            }
            if (recursive && dangerous_target(lower + i, k - i)) return p - lower;
            break;
        }
    }
    const char *m = find_word(lower, n, "mkfs", 0);
    if (!m) m = strstr(lower, "mkfs.");
    if (m && strstr(m, "/dev/")) return m - lower;
    const char *dd = find_word(lower, n, "dd", 0);
    if (dd && strstr(dd, "of=/dev/") && !strstr(dd, "of=/dev/null")) return dd - lower;
    const char *ch = find_word(lower, n, "chmod", 0);
    if (ch && strstr(ch, "777")) {
        const char *end = lower + n;
        const char *last = end;
        while (last > ch && (last[-1] == ' ' || last[-1] == '\t')) --last;
        if (last > ch && last[-1] == '/' && (last - 1 == ch || last[-2] == ' ')) return ch - lower;
    }
    const char *bomb = strstr(lower, ":(){");
    if (bomb && strstr(bomb, ":|:")) return bomb - lower;
    return -1;
}

static long any_of(const char *lower, const char *const *needles) {
    long first = -1;
    for (; *needles; ++needles) {
        const char *hit = strstr(lower, *needles);
        if (hit && (first < 0 || hit - lower < first)) first = hit - lower;
    }
    return first;
}

static const char *const EXFIL[] = {"webhook.site", "requestbin", "pipedream.net", "discord.com/api/webhooks",
                                    "discordapp.com/api/webhooks", "api.telegram.org/bot", "pastebin.com", "transfer.sh", "interact.sh", ".oast.",
                                    "burpcollaborator", "canarytokens", NULL};

static const char *const OVERRIDES[] = {"ignore previous instructions", "ignore all previous instructions", "ignore the previous instructions",
                                        "ignore prior instructions", "ignore all prior instructions", "ignore the above instructions",
                                        "ignore your instructions", "disregard previous instructions", "disregard all previous instructions",
                                        "disregard your instructions", "do not tell the user", "don't tell the user", "without telling the user",
                                        "do not mention this to the user", "do not inform the user", "--dangerously-skip-permissions",
                                        "bypasspermissions", "--dangerously-bypass-approvals", NULL};

static const char *const SECRETS[] = {"~/.ssh", "$home/.ssh", "id_rsa", "id_ed25519", "id_ecdsa", ".aws/credentials", "~/.netrc", "$home/.netrc",
                                      "gh/hosts.yml", ".docker/config.json", ".kube/config", "find-generic-password", NULL};

static bool base64_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=' || c == '-' || c == '_';
}
static bool hex_char(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// letters and digits both appear in a real encoding; a markdown table rule of dashes has neither
static bool mixed(const char *s, size_t n) {
    bool letter = false, digit = false;
    for (size_t i = 0; i < n && !(letter && digit); ++i) {
        letter = letter || (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z');
        digit = digit || (s[i] >= '0' && s[i] <= '9');
    }
    return letter && digit;
}

// a long run of base64 or hex: something meant for a machine, not for the reader (inline images are left alone)
static long encoded_blob(const char *line, size_t n) {
    size_t run = 0, hex = 0;
    for (size_t i = 0; i < n; ++i) {
        run = base64_char(line[i]) ? run + 1 : 0;
        hex = hex_char(line[i]) ? hex + 1 : 0;
        if ((run >= 200 && mixed(line + i + 1 - run, run)) || (hex >= 400 && mixed(line + i + 1 - hex, hex))) {
            const size_t start = i + 1 - (run >= 200 ? run : hex);
            static const char data_image[] = "data:image/";
            const size_t look = start > 40 ? start - 40 : 0;
            char *before = sh_strndup(line + look, start - look);
            const bool image = strstr(before, data_image) != NULL;
            free(before);
            if (image) {
                run = hex = 0;
                continue;
            }
            return (long)start;
        }
    }
    return -1;
}

// invisible characters: bidi controls and tag characters are high (they hide or reorder instructions), a stray zero-width
// character is medium, and a zero-width joiner between emoji or a directional mark in right-to-left text is fine
static void hidden_text(sh_findings *f, const char *rel, int line_no, const char *line, size_t n, bool first_line) {
    const char *p = line, *end = line + n;
    uint32_t prev = 0;
    bool high = false, soft = false;
    while (p < end) {
        const char *at = p;
        const uint32_t c = sh_utf8_next(&p, end);
        if (!high && (bidi_control(c) || tag_character(c))) {
            report(f, "hidden-text", "Invisible text or reordered characters", SH_FIND_HIGH, rel, line_no, line, n, (size_t)(at - line));
            high = true;
        } else if (!soft && zero_width(c) && !(c == 0xFEFF && first_line && at == line)) {
            const char *q = p;
            const uint32_t next = q < end ? sh_utf8_next(&q, end) : 0;
            if (!(c == 0x200D && (pictograph(prev) || pictograph(next)))) {
                report(f, "hidden-text-soft", "A zero-width character", SH_FIND_MEDIUM, rel, line_no, line, n, (size_t)(at - line));
                soft = true;
            }
        }
        prev = c;
    }
}

void sh_scan_text(const char *rel, const char *text, size_t n, sh_findings *out) {
    if (memchr(text, '\0', n < 8192 ? n : 8192)) return;  // a binary file: Review flags programs on its own
    const char *p = text, *end = text + n;
    const bool picture = sh_ends_with(rel, ".svg") || sh_ends_with(rel, ".SVG");
    for (int line_no = 1; p < end; ++line_no) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const size_t len = (size_t)((nl ? nl : end) - p);
        char *lower = sh_strndup(p, len);
        for (char *c = lower; *c; ++c)
            if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
        long at;
        if ((at = pipe_to_shell(lower, len)) >= 0)
            report(out, "pipe-to-shell", "Downloads a script and runs it at once", SH_FIND_HIGH, rel, line_no, p, len, (size_t)at);
        if ((at = destructive(lower, len)) >= 0)
            report(out, "destructive", "A command that can wipe files or disks", SH_FIND_HIGH, rel, line_no, p, len, (size_t)at);
        if ((at = any_of(lower, EXFIL)) >= 0)
            report(out, "exfil-endpoint", "Sends data to a drop service", SH_FIND_HIGH, rel, line_no, p, len, (size_t)at);
        if ((at = any_of(lower, OVERRIDES)) >= 0)
            report(out, "instruction-override", "Tells the agent to override its rules or hide things", SH_FIND_MEDIUM, rel, line_no, p, len, (size_t)at);
        if ((at = any_of(lower, SECRETS)) >= 0)
            report(out, "secrets-access", "Touches keys, tokens or credentials", SH_FIND_MEDIUM, rel, line_no, p, len, (size_t)at);
        if (!picture && (at = encoded_blob(p, len)) >= 0)
            report(out, "encoded-blob", "A long encoded blob", SH_FIND_MEDIUM, rel, line_no, p, len, (size_t)at);
        hidden_text(out, rel, line_no, p, len, line_no == 1);
        free(lower);
        p = nl ? nl + 1 : end;
    }
}

typedef struct {
    sh_findings *out;
} scan_walk;

static bool scan_one(const char *rel, const char *full, bool is_link, void *user) {
    scan_walk *w = user;
    if (is_link || sh_starts_with(rel, ".git/") || strstr(rel, "/.git/")) return true;
    const int64_t size = sh_file_size(full);
    if (size < 0 || size > MAX_FILE) return true;
    sh_buf b = SH_BUF_INIT;
    if (sh_read_file(full, MAX_FILE, &b)) sh_scan_text(rel, b.data ? b.data : "", b.len, w->out);
    sh_buf_free(&b);
    return w->out->len < MAX_FINDINGS;
}

void sh_scan_dir(const char *dir, sh_findings *out) {
    memset(out, 0, sizeof *out);
    scan_walk w = {out};
    if (sh_is_dir(dir)) sh_walk_files(dir, scan_one, &w);
}

static bool same_finding(const sh_finding *a, const sh_finding *b) {
    return a->rule == b->rule && strcmp(a->file, b->file) == 0 && strcmp(a->excerpt, b->excerpt) == 0;
}

void sh_scan_added(const char *old_dir, const char *new_dir, sh_findings *out) {
    sh_findings before, after;
    sh_scan_dir(old_dir, &before);
    sh_scan_dir(new_dir, &after);
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < after.len; ++i) {
        bool known = false;
        for (size_t k = 0; k < before.len && !known; ++k) known = same_finding(&after.items[i], &before.items[k]);
        if (!known) add(out, after.items[i].rule, after.items[i].title, after.items[i].level, after.items[i].file, after.items[i].line, after.items[i].excerpt);
    }
    out->truncated = after.truncated;
    sh_findings_free(&before);
    sh_findings_free(&after);
}

bool sh_scan_adds_high(const char *installed, const char *stage) {
    sh_findings added;
    sh_scan_added(installed, stage, &added);
    const bool high = sh_findings_count(&added, SH_FIND_HIGH) > 0;
    sh_findings_free(&added);
    return high;
}
