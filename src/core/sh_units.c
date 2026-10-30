// Notifications, and removing what Skill Hub put on the machine.
#include <stdlib.h>
#include <string.h>

#include "sh_exe.h"
#include "sh_fs.h"
#include "sh_path.h"
#include "sh_proc.h"
#include "sh_store.h"
#include "sh_utf.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

void sh_notify(const char *title, const char *body) {
#if defined(__linux__)
    char *notify_send = sh_find_executable("notify-send", NULL);
    if (notify_send) {
        const char *argv[] = {notify_send, "-a", "Skill Hub", "-i", "system-software-update", title, body, NULL};
        sh_proc_detach(argv);
        free(notify_send);
    }
#elif defined(__APPLE__)
    sh_buf script = SH_BUF_INIT;
    sh_buf_puts(&script, "display notification \"");
    for (const char *p = body; *p; ++p) {
        if (*p == '\\' || *p == '"') sh_buf_putc(&script, '\\');
        sh_buf_putc(&script, *p);
    }
    sh_buf_puts(&script, "\" with title \"");
    for (const char *p = title; *p; ++p) {
        if (*p == '\\' || *p == '"') sh_buf_putc(&script, '\\');
        sh_buf_putc(&script, *p);
    }
    sh_buf_puts(&script, "\"");
    const char *argv[] = {"osascript", "-e", sh_buf_str(&script), NULL};
    sh_proc_detach(argv);
    sh_buf_free(&script);
#elif defined(_WIN32)
    // a balloon from the tray, which needs a window to belong to; this process is about to end, so it waits for the balloon
    NOTIFYICONDATAW n;
    memset(&n, 0, sizeof n);
    n.cbSize = sizeof n;
    n.hWnd = CreateWindowExW(0, L"STATIC", L"Skill Hub", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
    n.uID = 1;
    n.uFlags = NIF_INFO | NIF_ICON | NIF_TIP;
    n.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
    if (!n.hIcon) n.hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(32516));  // IDI_INFORMATION, in its wide form
    n.dwInfoFlags = NIIF_INFO;
    MultiByteToWideChar(CP_UTF8, 0, title, -1, n.szInfoTitle, (int)(sizeof n.szInfoTitle / sizeof n.szInfoTitle[0]) - 1);
    MultiByteToWideChar(CP_UTF8, 0, body, -1, n.szInfo, (int)(sizeof n.szInfo / sizeof n.szInfo[0]) - 1);
    memcpy(n.szTip, L"Skill Hub", sizeof L"Skill Hub");
    if (Shell_NotifyIconW(NIM_ADD, &n)) {
        Sleep(6000);
        Shell_NotifyIconW(NIM_DELETE, &n);
    }
    if (n.hWnd) DestroyWindow(n.hWnd);
#else
    (void)title;
    (void)body;
#endif
}

// Remove what Skill Hub itself put on the machine: systemd units, installed files below $HOME, the git cache.
// Never touches skills, the lock file, agent links or the backup folder.
bool sh_store_uninstall(sh_store *st, bool dry_run, bool keep_cache) {
    const char *verb = dry_run ? "would remove" : "removed";
    bool ok = true;
#ifdef __linux__
    // 1. systemd user units
    char *d = sh_path_join(st->home, ".config/systemd/user");
    static const char *const unit_names[] = {"skillhub-update.timer", "skillhub-update.service", "skillhub-adopt.path",
                                             "skillhub-adopt.service"};
    sh_strv units = SH_STRV_INIT;
    for (size_t i = 0; i < 4; ++i) {
        char *f = sh_path_join(d, unit_names[i]);
        if (sh_exists(f)) sh_strv_push(&units, unit_names[i]);
        free(f);
    }
    if (units.len > 0 && !dry_run) sh_store_remove_units(st, true, true, NULL);
    for (size_t i = 0; i < units.len; ++i) {
        char *f = sh_path_join(d, units.items[i]);
        if (dry_run || !sh_exists(f)) {
            sh_store_log(st, "%s unit %s", verb, f);
        } else {
            sh_store_log(st, "FAILED to remove %s", f);
            ok = false;
        }
        free(f);
    }
    sh_strv_free(&units);
    free(d);

    // 2. installed files: only below $HOME (anything else belongs to a package manager or root)
    sh_strv prefixes = SH_STRV_INIT;
    sh_strv_take(&prefixes, sh_path_join(st->home, ".local"));
    char exe[4096];
    if (sh_exe_dir(exe, sizeof exe) && strcmp(sh_path_name(exe), "bin") == 0) {
        char *prefix = sh_path_parent(exe);
        if (!sh_strv_contains(&prefixes, prefix)) sh_strv_take(&prefixes, prefix);
        else free(prefix);
    }
    char *home_slash = sh_strfmt("%s/", st->home);
    for (size_t p = 0; p < prefixes.len; ++p) {
        const char *prefix = prefixes.items[p];
        sh_strv files = SH_STRV_INIT;
        sh_strv_take(&files, sh_strfmt("%s/bin/skillhub", prefix));
        sh_strv_take(&files, sh_strfmt("%s/bin/skillhub-gui", prefix));
        sh_strv_take(&files, sh_strfmt("%s/share/applications/skillhub.desktop", prefix));
        sh_strv_take(&files, sh_strfmt("%s/share/metainfo/io.github.nimonht.skillhub.metainfo.xml", prefix));
        sh_strv_take(&files, sh_strfmt("%s/share/icons/hicolor/scalable/apps/skillhub.svg", prefix));
        static const int sizes[] = {16, 24, 32, 48, 64, 128, 256, 512};
        for (size_t i = 0; i < 8; ++i)
            sh_strv_take(&files, sh_strfmt("%s/share/icons/hicolor/%dx%d/apps/skillhub.png", prefix, sizes[i], sizes[i]));
        bool any = false;
        for (size_t i = 0; i < files.len; ++i) any = any || sh_lexists(files.items[i]);
        if (any && !sh_starts_with(prefix, home_slash)) {
            sh_store_log(st, "not touching %s: installed outside $HOME, use your package manager or the build's uninstall target", prefix);
            any = false;
        }
        for (size_t i = 0; any && i < files.len; ++i) {
            const char *f = files.items[i];
            if (!sh_lexists(f)) continue;
            if (dry_run) {
                sh_store_log(st, "%s %s", verb, f);
            } else if (sh_remove_file(f)) {
                sh_store_log(st, "%s %s", verb, f);
            } else {
                sh_store_log(st, "FAILED to remove %s", f);
                ok = false;
            }
        }
        sh_strv_free(&files);
    }
    free(home_slash);
    sh_strv_free(&prefixes);
#else
    // 1. LaunchAgents or scheduled tasks; the application itself goes the way it came
    for (int timer = 1; timer >= 0; --timer) {
        char *state = sh_automation_state(st, timer != 0);
        if (strcmp(state, "not installed") != 0 && strcmp(state, "not available") != 0) {
            if (!dry_run) sh_store_remove_units(st, timer != 0, timer == 0, NULL);
            sh_store_log(st, "%s %s", verb, sh_automation_name(timer != 0));
        }
        free(state);
    }
    sh_store_log(st, "delete the application the way you installed it");
#endif

    // 3. the cache of cloned upstream repositories
    if (!keep_cache && strcmp(sh_path_name(st->cache), "skillhub") == 0 && sh_exists(st->cache)) {
        if (dry_run) {
            sh_store_log(st, "%s %s", verb, st->cache);
        } else if (sh_remove_tree(st->cache)) {
            sh_store_log(st, "%s %s", verb, st->cache);
        } else {
            sh_store_log(st, "FAILED to remove %s", st->cache);
            ok = false;
        }
    }
    char *providers = sh_store_providers_file();
    sh_store_log(st, "kept: %s, %s, agent links, %s, %s", st->central, st->lock_path, st->backup, providers);
    free(providers);
    return ok;
}
