// See sh_selfupdate.h. SECURITY.md says what this does with the network, and why a release must be signed.
#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE
#endif

#include "sh_selfupdate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "monocypher-ed25519.h"
#include "sh_exe.h"
#include "sh_fs.h"
#include "sh_git.h"
#include "sh_hash.h"
#include "sh_http.h"
#include "sh_path.h"
#include "sh_proc.h"
#include "sh_state.h"
#include "sh_utf.h"

#define PROJECT "https://github.com/nimonht/skillhub"
#define MAX_CANDIDATES 3          // tags that may lack a published, signed release yet (CI takes a while) before giving up
#define MAX_SUMS (1024 * 1024)
#define SMALL_FILE_MS 30000
#define BIG_FILE_MS (20 * 60 * 1000)

// ---- versions ----

typedef struct {
    long major, minor, patch;
    char label[16];  // "" for a final version
    long number;
} version;

static bool number_of(const char **p, long *out) {
    const char *s = *p;
    long n = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
        if (++digits > 9) return false;
        n = n * 10 + (*s++ - '0');
    }
    if (!digits) return false;
    *p = s;
    *out = n;
    return true;
}

static bool parse_version(const char *s, version *v) {
    memset(v, 0, sizeof *v);
    if (!number_of(&s, &v->major) || *s != '.') return false;
    ++s;
    if (!number_of(&s, &v->minor) || *s != '.') return false;
    ++s;
    if (!number_of(&s, &v->patch)) return false;
    if (!*s) return true;
    if (*s != '-') return false;
    ++s;
    size_t n = 0;
    while (*s >= 'a' && *s <= 'z') {
        if (n + 1 >= sizeof v->label) return false;
        v->label[n++] = *s++;
    }
    if (!n) return false;
    return !*s || (number_of(&s, &v->number) && !*s);
}

bool sh_version_valid(const char *v) {
    version parsed;
    return parse_version(v, &parsed);
}

static int compare_long(long a, long b) { return a < b ? -1 : a > b; }

int sh_version_compare(const char *a, const char *b) {
    version x, y;
    const bool ok_x = parse_version(a, &x), ok_y = parse_version(b, &y);
    if (!ok_x || !ok_y) return (int)ok_x - (int)ok_y;
    int c = compare_long(x.major, y.major);
    if (!c) c = compare_long(x.minor, y.minor);
    if (!c) c = compare_long(x.patch, y.patch);
    if (c) return c;
    if (!*x.label || !*y.label) return (int)!*x.label - (int)!*y.label;  // the final version comes after its pre-releases
    c = strcmp(x.label, y.label);
    return c ? c : compare_long(x.number, y.number);
}

static int newest_first(const char *a, const char *b) { return sh_version_compare(b, a); }

const char *sh_update_channel_name(sh_update_channel channel) { return channel == SH_CHANNEL_BETA ? "beta" : "stable"; }

sh_update_channel sh_update_channel_of(const char *running, const char *chosen) {
    if (chosen && strcmp(chosen, "beta") == 0) return SH_CHANNEL_BETA;
    if (chosen && strcmp(chosen, "stable") == 0) return SH_CHANNEL_STABLE;
    version me;
    return parse_version(running, &me) && *me.label ? SH_CHANNEL_BETA : SH_CHANNEL_STABLE;
}

void sh_update_candidates(const sh_strv *tags, const char *running, sh_update_channel channel, size_t max, sh_strv *out) {
    version me;
    if (!parse_version(running, &me) || strcmp(me.label, "dev") == 0) return;
    sh_strv newer = SH_STRV_INIT, back = SH_STRV_INIT;
    for (size_t i = 0; i < tags->len; ++i) {
        const char *tag = sh_starts_with(tags->items[i], "refs/tags/") ? tags->items[i] + 10 : tags->items[i];
        version v;
        if (*tag != 'v' || !parse_version(tag + 1, &v) || strcmp(v.label, "dev") == 0) continue;
        if (*v.label && channel == SH_CHANNEL_STABLE) continue;  // stable never lands on a pre-release
        const int c = sh_version_compare(tag + 1, running);
        // going back is for a pre-release only, to a final version that can itself come forward again
        const bool may_go_back = c < 0 && *me.label && channel == SH_CHANNEL_STABLE && sh_version_compare(tag + 1, SH_UPDATE_FLOOR) >= 0;
        sh_strv *list = c > 0 ? &newer : may_go_back ? &back : NULL;
        if (list && !sh_strv_contains(list, tag + 1)) sh_strv_push(list, tag + 1);
    }
    const sh_strv *found = newer.len ? &newer : &back;  // while anything newer is out, nothing goes back
    sh_strv_sort(&newer, newest_first);
    sh_strv_sort(&back, newest_first);
    for (size_t i = 0; i < found->len && i < max; ++i) sh_strv_push(out, found->items[i]);
    sh_strv_free(&newer);
    sh_strv_free(&back);
}

// ---- signatures ----

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool sh_update_signature_ok(const void *sums, size_t sums_len, const void *signature, size_t signature_len, const char *public_key) {
    if (signature_len != 64 || strlen(public_key) != 64) return false;
    uint8_t key[32];
    for (size_t i = 0; i < sizeof key; ++i) {
        const int hi = hex_digit(public_key[2 * i]), lo = hex_digit(public_key[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        key[i] = (uint8_t)(hi << 4 | lo);
    }
    return crypto_ed25519_check(signature, key, sums, sums_len) == 0;
}

bool sh_update_sum_of(const char *sums, const char *name, char out[65]) {
    sh_strv lines = SH_STRV_INIT;
    sh_split(sums, strlen(sums), '\n', true, &lines);
    bool found = false;
    for (size_t i = 0; i < lines.len && !found; ++i) {
        const char *line = lines.items[i];
        size_t n = strlen(line);
        if (n && line[n - 1] == '\r') --n;
        size_t digits = 0;
        while (digits < n && hex_digit(line[digits]) >= 0) ++digits;
        if (digits != 64 || n < 66 || line[64] != ' ') continue;
        const char *file = line + 65;  // sha256sum writes two spaces, or a space and a star
        if (*file == ' ' || *file == '*') ++file;
        const size_t len = (size_t)(line + n - file);
        if (len != strlen(name) || memcmp(file, name, len) != 0) continue;
        for (size_t k = 0; k < 64; ++k) out[k] = line[k] >= 'A' && line[k] <= 'F' ? (char)(line[k] + 32) : line[k];
        out[64] = '\0';
        found = true;
    }
    sh_strv_free(&lines);
    return found;
}

// ---- what kind of install this is ----

const char *sh_update_kind_name(sh_install_kind kind) {
    switch (kind) {
    case SH_INSTALL_PACKAGE: return "package";
    case SH_INSTALL_ZIP: return "zip";
    case SH_INSTALL_SETUP: return "setup";
    case SH_INSTALL_APP: return "app";
    case SH_INSTALL_PORTABLE: return "portable";
    default: return "source";
    }
}

#ifdef __APPLE__
// "<...>.app" for "<...>.app/Contents/MacOS/skillhub", else NULL
static char *app_bundle(const char *exe) {
    char *macos = sh_path_parent(exe), *contents = sh_path_parent(macos), *app = sh_path_parent(contents);
    const bool ok = strcmp(sh_path_name(macos), "MacOS") == 0 && strcmp(sh_path_name(contents), "Contents") == 0 && sh_ends_with(app, ".app");
    free(macos);
    free(contents);
    if (!ok) free(app);
    return ok ? app : NULL;
}
#endif

#if defined(__linux__)
// "<root>" for "<root>/<version>/bin/skillhub" when <root>/current exists: what packaging/linux/install.sh sets up
static char *portable_root(const char *exe) {
    char *bin = sh_path_parent(exe), *folder = sh_path_parent(bin), *root = sh_path_parent(folder);
    char *current = sh_path_join(root, "current");
    const bool ok = strcmp(sh_path_name(bin), "bin") == 0 && sh_is_dir(current);
    free(bin);
    free(folder);
    free(current);
    if (!ok) free(root);
    return ok ? root : NULL;
}
#endif

sh_install_kind sh_update_detect(const sh_update_build *b, const char *exe) {
#if defined(_WIN32)
    if (!b->release_build) return SH_INSTALL_SOURCE;
    char *dir = sh_path_parent(exe), *uninstaller = sh_path_join(dir, "unins000.exe");
    const bool setup = sh_is_file(uninstaller);
    free(dir);
    free(uninstaller);
    return setup ? SH_INSTALL_SETUP : SH_INSTALL_ZIP;
#elif defined(__APPLE__)
    char *app = b->release_build ? app_bundle(exe) : NULL;
    const bool found = app != NULL;
    free(app);
    return found ? SH_INSTALL_APP : SH_INSTALL_SOURCE;
#else
    if (sh_starts_with(exe, "/usr/") && !sh_starts_with(exe, "/usr/local/")) return SH_INSTALL_PACKAGE;
#if defined(__linux__)
    char *root = b->release_build ? portable_root(exe) : NULL;
    const bool found = root != NULL;
    free(root);
    if (found) return SH_INSTALL_PORTABLE;
#else
    (void)b;
#endif
    return SH_INSTALL_SOURCE;
#endif
}

// with forward slashes, which is what the path functions split on: on Windows sh_exe_path gives backslashes
static char *running_program(void) {
    char exe[4096];
    return sh_exe_path(exe, sizeof exe) ? sh_path_clean(exe) : sh_strdup("");
}

// the file a release holds for this install, NULL when it has none to replace it with
static char *asset_for(sh_install_kind kind, const char *v) {
    switch (kind) {
    case SH_INSTALL_SETUP: return sh_strfmt("SkillHub-%s-windows-x64-setup.exe", v);
    case SH_INSTALL_APP: return sh_strfmt("SkillHub-%s-macos-universal.dmg", v);
#if defined(__x86_64__) || defined(__aarch64__)
#if defined(__x86_64__)
    case SH_INSTALL_PORTABLE: return sh_strfmt("skillhub-%s-linux-x86_64.tar.gz", v);
#else
    case SH_INSTALL_PORTABLE: return sh_strfmt("skillhub-%s-linux-aarch64.tar.gz", v);
#endif
#endif
    default: return NULL;
    }
}

// ---- network ----

static char *env_or(const char *name, const char *fallback) {
    char *v = sh_getenv(name);  // the tests' own server; what comes from it must still carry the signature
    if (v && *v) return v;
    free(v);
    return sh_strdup(fallback);
}

static bool remote_versions(sh_strv *tags, char **err) {
    char *remote = env_or("SKILLHUB_UPDATE_REMOTE", PROJECT ".git");
    const char *const args[] = {"ls-remote", "--tags", "--refs", "--", remote, NULL};
    sh_git_result g = sh_git_run(args, NULL, SMALL_FILE_MS);
    if (g.ok) {
        sh_strv lines = SH_STRV_INIT;
        sh_split(sh_buf_str(&g.out), g.out.len, '\n', true, &lines);
        for (size_t i = 0; i < lines.len; ++i) {
            const char *tab = strchr(lines.items[i], '\t');
            if (tab) sh_strv_push(tags, tab + 1);
        }
        sh_strv_free(&lines);
    } else {
        *err = sh_strfmt("could not reach the project's repository: %s", g.err ? g.err : "");
    }
    const bool ok = g.ok;
    sh_git_result_free(&g);
    free(remote);
    return ok;
}

// SHA256SUMS of `v`, in `dir`, once its signature checks out
static bool signed_sums(const sh_update_build *b, const char *v, const char *dir, sh_buf *sums, char **err) {
    char *base = env_or("SKILLHUB_UPDATE_BASE", PROJECT "/releases/download");
    char *sums_url = sh_strfmt("%s/v%s/SHA256SUMS", base, v), *sig_url = sh_strfmt("%s/v%s/SHA256SUMS.sig", base, v);
    char *sums_file = sh_path_join(dir, "SHA256SUMS"), *sig_file = sh_path_join(dir, "SHA256SUMS.sig");
    sh_buf sig = SH_BUF_INIT;
    bool ok = sh_mkpath(dir);
    if (!ok) *err = sh_strfmt("cannot create %s", dir);
    ok = ok && sh_http_get(sig_url, sig_file, SMALL_FILE_MS, err) && sh_http_get(sums_url, sums_file, SMALL_FILE_MS, err);
    if (ok) {
        ok = sh_read_file(sums_file, MAX_SUMS, sums) && sh_read_file(sig_file, 1024, &sig) &&
             sh_update_signature_ok(sums->data, sums->len, sig.data, sig.len, b->public_key);
        if (!ok) *err = sh_strfmt("the signature of version %s is not valid", v);
    }
    sh_buf_free(&sig);
    free(sums_file);
    free(sig_file);
    free(sums_url);
    free(sig_url);
    free(base);
    return ok;
}

static char *download_dir(const char *cache, const char *v) {
    char *top = sh_path_join(cache, "self-update"), *dir = sh_path_join(top, v);
    free(top);
    return dir;
}

// ---- looking for a new version ----

void sh_update_info_free(sh_update_info *u) {
    free(u->current);
    free(u->latest);
    free(u->page);
    free(u->advice);
    free(u->note);
    memset(u, 0, sizeof *u);
}

void sh_update_info_json(yyjson_mut_doc *d, yyjson_mut_val *o, const sh_update_info *u) {
    yyjson_mut_obj_add_strcpy(d, o, "current", u->current);
    yyjson_mut_obj_add_strcpy(d, o, "latest", u->latest);
    yyjson_mut_obj_add_bool(d, o, "available", u->available);
    yyjson_mut_obj_add_bool(d, o, "canInstall", u->can_install);
    yyjson_mut_obj_add_strcpy(d, o, "kind", sh_update_kind_name(u->kind));
    yyjson_mut_obj_add_strcpy(d, o, "page", u->page);
    yyjson_mut_obj_add_strcpy(d, o, "advice", u->advice);
    yyjson_mut_obj_add_strcpy(d, o, "note", u->note);
    yyjson_mut_obj_add_str(d, o, "channel", u->channel ? u->channel : "stable");
    yyjson_mut_obj_add_bool(d, o, "downgrade", u->downgrade);
}

#if defined(__linux__)
static const char *package_command(void) {
    if (sh_exists("/etc/debian_version")) return "sudo apt install ./skillhub_*.deb";
    if (sh_exists("/etc/fedora-release")) return "sudo dnf install ./skillhub-*.rpm";
    if (sh_exists("/etc/arch-release")) return "makepkg -si, in packaging/arch of the new version";
    return NULL;
}
#endif

// why this install cannot replace itself with `v`, "" when it can
static char *blocked(const sh_update_build *b, sh_install_kind kind, const char *exe, const char *v, const char *page) {
    (void)exe;
    switch (kind) {
    case SH_INSTALL_PACKAGE: {
#if defined(__linux__)
        const char *command = package_command();
        if (command) return sh_strfmt("Installed as a system package, so it cannot replace itself. Get the package of %s from %s, then run: %s", v, page, command);
#endif
        return sh_strfmt("Installed as a system package, so it cannot replace itself. Get the package of %s from %s", v, page);
    }
    case SH_INSTALL_ZIP: return sh_strfmt("The portable version does not replace itself. Get the zip of %s from %s", v, page);
    case SH_INSTALL_SOURCE: return sh_strfmt("Built from source: check out v%s, rebuild and install again, or use a package from %s", v, page);
    default: break;
    }
    if (!*b->public_key) return sh_strfmt("This build cannot verify other versions. Get %s from %s", v, page);
    if (!sh_http_available()) return sh_strfmt("Neither curl nor wget was found, so it cannot download the update. Install one of them, or get it from %s", page);
#ifdef __APPLE__
    if (kind == SH_INSTALL_APP) {
        if (strstr(exe, "/AppTranslocation/")) return sh_strdup("Move Skill Hub to the Applications folder first, then update.");
        char *app = app_bundle(exe), *parent = app ? sh_path_parent(app) : sh_strdup("");
        const bool writable = access(parent, W_OK) == 0;
        free(app);
        free(parent);
        if (!writable) return sh_strfmt("This account cannot change the folder Skill Hub is in. Get %s from %s", v, page);
    }
#endif
    return sh_strdup("");
}

static void set_latest(sh_update_info *u, const char *v) {
    free(u->latest);
    free(u->page);
    u->latest = sh_strdup(v);
    u->page = sh_strfmt(PROJECT "/releases/tag/v%s", v);
    u->available = true;
    u->downgrade = sh_version_compare(v, u->current) < 0;
}

bool sh_update_check(const sh_update_build *b, const char *cache, const char *chosen, sh_update_info *out, char **err) {
    memset(out, 0, sizeof *out);
    char *exe = running_program();
    out->kind = sh_update_detect(b, exe);
    out->current = sh_strdup(b->version);
    out->latest = sh_strdup("");
    out->page = sh_strdup("");
    out->advice = sh_strdup("");
    out->note = sh_strdup("");
    char *saved = chosen ? NULL : sh_setting_str("updates", "channel");
    const sh_update_channel channel = sh_update_channel_of(b->version, chosen ? chosen : saved);
    free(saved);
    out->channel = sh_update_channel_name(channel);
    sh_strv tags = SH_STRV_INIT, candidates = SH_STRV_INIT;
    const bool reached = remote_versions(&tags, err);
    if (reached) sh_update_candidates(&tags, b->version, channel, MAX_CANDIDATES, &candidates);
    if (candidates.len) {
        char *page = sh_strfmt(PROJECT "/releases/tag/v%s", candidates.items[0]);
        char *why = blocked(b, out->kind, exe, candidates.items[0], page);
        free(page);
        if (*why) {
            set_latest(out, candidates.items[0]);
            free(out->advice);
            out->advice = why;
            why = NULL;
        }
        free(why);
        for (size_t i = 0; i < candidates.len && !out->available; ++i) {
            char *dir = download_dir(cache, candidates.items[i]), *asset = asset_for(out->kind, candidates.items[i]), *why_not = NULL;
            sh_buf sums = SH_BUF_INIT;
            char sum[65];
            if (!signed_sums(b, candidates.items[i], dir, &sums, &why_not)) {
                if (!*out->note) {
                    free(out->note);
                    out->note = sh_strfmt("%s was passed over: %s", candidates.items[i], why_not ? why_not : "");
                }
            } else if (asset && sh_update_sum_of(sh_buf_str(&sums), asset, sum)) {
                set_latest(out, candidates.items[i]);
                out->can_install = true;
            } else if (!*out->note) {
                free(out->note);
                out->note = sh_strfmt("%s was passed over: it has no %s", candidates.items[i], asset ? asset : "file for this system");
            }
            sh_buf_free(&sums);
            free(why_not);
            free(asset);
            free(dir);
        }
    }
    sh_strv_free(&tags);
    sh_strv_free(&candidates);
    free(exe);
    return reached;
}

// ---- installing ----

#if defined(__linux__) || defined(__APPLE__)
static bool run_tool(const char *const *argv, int timeout_ms, char **err) {
    sh_proc_opts o = {0};
    o.timeout_ms = timeout_ms;
    sh_proc_result r = {0};
    const bool ok = sh_proc_run(argv, &o, &r);
    if (!ok && err && !*err) {
        char *why = sh_trimmed(sh_buf_str(&r.err), r.err.len);
        *err = sh_strfmt("%s failed: %s", sh_path_name(argv[0]), r.started ? why : "it could not be started");
        free(why);
    }
    sh_proc_result_free(&r);
    return ok;
}

// Before anything is replaced: the new program must start, and be the version the signature was for.
static bool runs_as(const char *program, const char *v, char **err) {
    const char *const argv[] = {program, "--version", NULL};
    sh_proc_opts o = {0};
    o.timeout_ms = SMALL_FILE_MS;
    sh_proc_result r = {0};
    bool ok = sh_proc_run(argv, &o, &r);
    char *said = sh_trimmed(sh_buf_str(&r.out), r.out.len), *want = sh_strfmt("skillhub %s", v);
    ok = ok && strcmp(said, want) == 0;
    if (!ok) *err = sh_strfmt("the new version does not start, so nothing was changed (it said: %s)", said);
    free(said);
    free(want);
    sh_proc_result_free(&r);
    return ok;
}
#endif

#if defined(__linux__)
// <root>/current is a link to a version's folder, so moving to the new version is one atomic rename, and the version running
// now stays in place until the next update.
static bool point_current(const char *root, const char *v, char **err) {
    char *link = sh_strfmt("%s/.current-%ld", root, sh_process_id()), *current = sh_path_join(root, "current");
    unlink(link);
    const bool ok = symlink(v, link) == 0 && rename(link, current) == 0;
    if (!ok) {
        *err = sh_strfmt("could not switch %s to the new version", current);
        unlink(link);
    }
    free(link);
    free(current);
    return ok;
}

static void keep_only(const char *root, const char *keep_a, const char *keep_b) {
    sh_strv names = SH_STRV_INIT;
    sh_list_dir(root, 0, &names);
    for (size_t i = 0; i < names.len; ++i) {
        const char *name = names.items[i];
        if (strcmp(name, "current") == 0 || strcmp(name, keep_a) == 0 || strcmp(name, keep_b) == 0) continue;
        char *path = sh_path_join(root, name);
        sh_remove_tree(path);
        free(path);
    }
    sh_strv_free(&names);
}

static bool apply_portable(const char *exe, const char *file, const char *v, bool relaunch, sh_strv *then, char **err) {
    char *root = portable_root(exe);
    if (!root) {
        *err = sh_strdup("this copy is not the portable version any more");
        return false;
    }
    char *fresh = sh_strfmt("%s/.new-%ld", root, sh_process_id()), *target = sh_path_join(root, v);
    char *program = sh_path_join3(fresh, "bin", "skillhub");
    const char *const tar[] = {"tar", "-xzf", file, "--strip-components=1", "-C", fresh, NULL};
    sh_remove_tree(fresh);
    bool ok = sh_mkpath(fresh);
    if (!ok) *err = sh_strfmt("cannot write to %s", root);
    ok = ok && run_tool(tar, BIG_FILE_MS, err) && runs_as(program, v, err);
    if (ok) {
        sh_remove_tree(target);  // an earlier try, or the version before this one; never the running one, which is another version
        ok = sh_rename(fresh, target);
        if (!ok) *err = sh_strfmt("could not move the new version to %s", target);
    }
    ok = ok && point_current(root, v, err);
    if (ok) {
        char *bin = sh_path_parent(exe), *running = sh_path_parent(bin);
        keep_only(root, v, sh_path_name(running));
        free(bin);
        free(running);
        if (relaunch) sh_strv_take(then, sh_path_join3(target, "bin", "skillhub-gui"));
    }
    sh_remove_tree(fresh);
    free(program);
    free(fresh);
    free(target);
    free(root);
    return ok;
}
#endif

#ifdef __APPLE__
static bool apply_app(const char *exe, const char *file, const char *dir, const char *v, bool relaunch, sh_strv *then, char **err) {
    char *app = app_bundle(exe);
    if (!app) {
        *err = sh_strdup("this copy is not in an app bundle any more");
        return false;
    }
    char *parent = sh_path_parent(app), *mount = sh_path_join(dir, "mount"), *source = sh_path_join(mount, "Skill Hub.app");
    char *fresh = sh_strfmt("%s/.%s.new-%ld", parent, sh_path_name(app), sh_process_id());
    char *old = sh_strfmt("%s/.%s.old-%ld", parent, sh_path_name(app), sh_process_id());
    char *program = sh_path_join3(fresh, "Contents/MacOS", "skillhub");
    const char *const attach[] = {"/usr/bin/hdiutil", "attach", file, "-nobrowse", "-readonly", "-noverify", "-noautoopen", "-mountpoint", mount, NULL};
    const char *const copy[] = {"/usr/bin/ditto", source, fresh, NULL};
    const char *const detach[] = {"/usr/bin/hdiutil", "detach", mount, "-force", NULL};
    sh_remove_tree(fresh);
    bool ok = sh_mkpath(mount) && run_tool(attach, 120000, err);
    if (ok) {
        ok = run_tool(copy, BIG_FILE_MS, err);
        run_tool(detach, 60000, NULL);
    }
    ok = ok && runs_as(program, v, err);
    if (ok) {
        ok = rename(app, old) == 0;
        if (ok && rename(fresh, app) != 0) {
            ok = false;
            rename(old, app);
        }
        if (!ok) *err = sh_strfmt("could not replace %s", app);
    }
    sh_remove_tree(fresh);
    if (ok && relaunch) {
        sh_strv_push(then, "/usr/bin/open");
        sh_strv_push(then, "-n");
        sh_strv_push(then, app);
    }
    free(program);
    free(old);
    free(fresh);
    free(source);
    free(mount);
    free(parent);
    free(app);
    return ok;
}
#endif

bool sh_update_install(const sh_update_build *b, const sh_update_info *u, const char *cache, bool relaunch, sh_update_log log, void *user,
                       sh_strv *then, char **err) {
    if (!u->available || !u->can_install) {
        *err = sh_strdup(*u->advice ? u->advice : "no version to install");
        return false;
    }
    char *asset = asset_for(u->kind, u->latest), *dir = download_dir(cache, u->latest), *exe = running_program();
    char *file = asset ? sh_path_join(dir, asset) : NULL;
    char *base = env_or("SKILLHUB_UPDATE_BASE", PROJECT "/releases/download");
    char *url = sh_strfmt("%s/v%s/%s", base, u->latest, asset ? asset : "");
    sh_buf sums = SH_BUF_INIT;
    char want[65], got[65] = "";
    bool ok = asset != NULL && signed_sums(b, u->latest, dir, &sums, err) && sh_update_sum_of(sh_buf_str(&sums), asset, want);
    if (ok) {
        log(user, "Downloading Skill Hub");
        ok = sh_http_get(url, file, BIG_FILE_MS, err);
    } else if (!*err) {
        *err = sh_strfmt("version %s has nothing for this system", u->latest);
    }
    if (ok) {
        sh_sha256 c;
        unsigned char digest[32];
        sh_sha256_init(&c);
        sh_sha256_add_file(&c, file);
        sh_sha256_final(&c, digest);
        sh_hex(digest, sizeof digest, got);
        ok = strcmp(got, want) == 0;
        if (!ok) {
            *err = sh_strfmt("%s does not match its signed checksum, so it was thrown away", asset);
            sh_remove_file(file);
        }
    }
    if (ok) {
        log(user, "Installing Skill Hub");
        switch (u->kind) {
#if defined(__linux__)
        case SH_INSTALL_PORTABLE: ok = apply_portable(exe, file, u->latest, relaunch, then, err); break;
#endif
#ifdef __APPLE__
        case SH_INSTALL_APP: ok = apply_app(exe, file, dir, u->latest, relaunch, then, err); break;
#endif
        case SH_INSTALL_SETUP: {
            // /CLOSEAPPLICATIONS: a background task may still hold the folder. The installer starts the window again on /RELAUNCH=1.
            sh_strv_push(then, file);
            static const char *const flags[] = {"/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CLOSEAPPLICATIONS", "/FORCECLOSEAPPLICATIONS"};
            for (size_t i = 0; i < sizeof flags / sizeof flags[0]; ++i) sh_strv_push(then, flags[i]);
            if (relaunch) sh_strv_push(then, "/RELAUNCH=1");
            break;
        }
        default:
            *err = sh_strdup("this install cannot replace itself");
            ok = false;
        }
    }
    sh_buf_free(&sums);
    free(url);
    free(base);
    free(file);
    free(exe);
    free(dir);
    free(asset);
    return ok;
}

void sh_update_tidy(const char *cache) {
    char *top = sh_path_join(cache, "self-update");
    sh_remove_tree(top);
    free(top);
#ifdef __APPLE__
    char *exe = running_program(), *app = app_bundle(exe);
    if (app) {
        char *parent = sh_path_parent(app), *prefix = sh_strfmt(".%s.old-", sh_path_name(app));
        sh_strv names = SH_STRV_INIT;
        sh_list_dir(parent, 0, &names);
        for (size_t i = 0; i < names.len; ++i) {
            if (!sh_starts_with(names.items[i], prefix)) continue;
            char *path = sh_path_join(parent, names.items[i]);
            sh_remove_tree(path);
            free(path);
        }
        sh_strv_free(&names);
        free(prefix);
        free(parent);
    }
    free(app);
    free(exe);
#endif
}
