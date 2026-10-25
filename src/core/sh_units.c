// Automation (the daily update timer and the stray watcher) and uninstall. Linux uses systemd user units, written exactly as
// 1.x wrote them; launchd and Task Scheduler arrive with milestone M3e of the 2.0 plan.
#include <stdlib.h>
#include <string.h>

#include "sh_exe.h"
#include "sh_fs.h"
#include "sh_path.h"
#include "sh_proc.h"
#include "sh_store.h"
#include "sh_utf.h"

bool sh_automation_supported(void) {
#ifdef __linux__
    return true;
#else
    return false;
#endif
}

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
#else
    (void)title;
    (void)body;
#endif
}

#ifdef __linux__

static char *unit_dir(const sh_store *st) { return sh_path_join(st->home, ".config/systemd/user"); }

static bool systemctl(const char *a, const char *b, const char *c, char **out, int timeout_ms) {
    const char *argv[] = {"systemctl", "--user", a, b, c, NULL};
    const sh_proc_opts o = {NULL, NULL, NULL, 0, timeout_ms ? timeout_ms : 60000};
    sh_proc_result r;
    const bool ok = sh_proc_run(argv, &o, &r);
    if (out) *out = sh_trimmed(sh_buf_str(&r.out), r.out.len);
    sh_proc_result_free(&r);
    return ok;
}

// The units run the command line: ~/.local/bin/skillhub when installed there, else this very program.
static char *exe_for_units(const sh_store *st) {
    char *installed = sh_path_join(st->home, ".local/bin/skillhub");
    const bool have = sh_exists(installed);
    free(installed);
    if (have) return sh_strdup("%h/.local/bin/skillhub");
    char self[4096];
    return sh_exe_path(self, sizeof self) ? sh_strdup(self) : sh_strdup("skillhub");
}

char *sh_store_unit_state(const sh_store *st, const char *unit) {
    (void)st;
    char *out = NULL;
    const char *argv[] = {"systemctl", "--user", "is-enabled", unit, NULL};
    const sh_proc_opts o = {NULL, NULL, NULL, 0, 8000};
    sh_proc_result r;
    sh_proc_run(argv, &o, &r);
    out = sh_trimmed(sh_buf_str(&r.out), r.out.len);
    sh_proc_result_free(&r);
    if (!*out) {
        free(out);
        return sh_strdup("not installed");
    }
    return out;
}

bool sh_store_install_units(sh_store *st, bool timer, bool watcher, char **err) {
    char *exe = exe_for_units(st);
    char *d = unit_dir(st);
    bool ok = true;
    if (timer) {
        char *service = sh_strfmt("[Unit]\nDescription=Update shared agent skills\nAfter=network-online.target\n\n"
                                  "[Service]\nType=oneshot\nExecStart=%s update --all --auto --notify\nNice=10\n", exe);
        const char *timer_text = "[Unit]\nDescription=Daily agent skill update\n\n"
                                 "[Timer]\nOnCalendar=daily\nPersistent=true\nRandomizedDelaySec=30min\nOnStartupSec=10min\n\n"
                                 "[Install]\nWantedBy=timers.target\n";
        char *f1 = sh_path_join(d, "skillhub-update.service"), *f2 = sh_path_join(d, "skillhub-update.timer");
        ok = sh_write_atomic(f1, service, strlen(service)) && ok;
        ok = sh_write_atomic(f2, timer_text, strlen(timer_text)) && ok;
        free(f1);
        free(f2);
        free(service);
    }
    if (watcher) {
        sh_strv dirs = SH_STRV_INIT;
        sh_store_watch_dirs(st, &dirs);
        if (dirs.len == 0) {
            if (err) *err = sh_strdup("no coding agents are managed yet, so there is nothing to watch");
            sh_strv_free(&dirs);
            free(d);
            free(exe);
            return false;
        }
        sh_buf paths = SH_BUF_INIT;
        for (size_t i = 0; i < dirs.len; ++i) sh_buf_printf(&paths, "PathChanged=%s\n", dirs.items[i]);
        sh_strv_free(&dirs);
        char *service = sh_strfmt("[Unit]\nDescription=Pull stray agent skills into ~/.agents/skills\n"
                                  "StartLimitIntervalSec=0\n\n"
                                  "[Service]\nType=oneshot\nExecStart=%s adopt --notify\n", exe);
        char *path_unit = sh_strfmt("[Unit]\nDescription=Watch agent skill dirs for stray skills\n\n"
                                    "[Path]\n%s\n"
                                    "[Install]\nWantedBy=default.target\n", sh_buf_str(&paths));
        char *f1 = sh_path_join(d, "skillhub-adopt.service"), *f2 = sh_path_join(d, "skillhub-adopt.path");
        ok = sh_write_atomic(f1, service, strlen(service)) && ok;
        ok = sh_write_atomic(f2, path_unit, strlen(path_unit)) && ok;
        free(f1);
        free(f2);
        free(service);
        free(path_unit);
        sh_buf_free(&paths);
    }
    if (!ok) {
        if (err) *err = sh_strfmt("cannot write unit files in %s", d);
        free(d);
        free(exe);
        return false;
    }
    systemctl("daemon-reload", NULL, NULL, NULL, 0);
    if (timer) ok = systemctl("enable", "--now", "skillhub-update.timer", NULL, 0) && ok;
    if (watcher) ok = systemctl("enable", "--now", "skillhub-adopt.path", NULL, 0) && ok;
    if (!ok && err) *err = sh_strdup("systemctl --user enable failed");
    free(d);
    free(exe);
    return ok;
}

bool sh_store_remove_units(sh_store *st, bool timer, bool watcher, char **err) {
    (void)err;
    char *d = unit_dir(st);
    if (timer) {
        systemctl("disable", "--now", "skillhub-update.timer", NULL, 0);
        char *f1 = sh_path_join(d, "skillhub-update.timer"), *f2 = sh_path_join(d, "skillhub-update.service");
        sh_remove_file(f1);
        sh_remove_file(f2);
        free(f1);
        free(f2);
    }
    if (watcher) {
        systemctl("disable", "--now", "skillhub-adopt.path", NULL, 0);
        char *f1 = sh_path_join(d, "skillhub-adopt.path"), *f2 = sh_path_join(d, "skillhub-adopt.service");
        sh_remove_file(f1);
        sh_remove_file(f2);
        free(f1);
        free(f2);
    }
    systemctl("daemon-reload", NULL, NULL, NULL, 0);
    free(d);
    return true;
}

#else

static const char NO_UNITS[] = "daily updates and the stray watcher need systemd user units, which are only available on Linux";

char *sh_store_unit_state(const sh_store *st, const char *unit) {
    (void)st;
    (void)unit;
    return sh_strdup("not available");
}

bool sh_store_install_units(sh_store *st, bool timer, bool watcher, char **err) {
    (void)st;
    (void)timer;
    (void)watcher;
    if (err) *err = sh_strdup(NO_UNITS);
    return false;
}

bool sh_store_remove_units(sh_store *st, bool timer, bool watcher, char **err) {
    (void)st;
    (void)timer;
    (void)watcher;
    (void)err;
    return true;
}

#endif

// Remove what Skill Hub itself put on the machine: systemd units, installed files below $HOME, the git cache.
// Never touches skills, the lock file, agent links or the backup folder.
bool sh_store_uninstall(sh_store *st, bool dry_run, bool keep_cache) {
    const char *verb = dry_run ? "would remove" : "removed";
    bool ok = true;
#ifdef __linux__
    // 1. systemd user units
    char *d = unit_dir(st);
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
    sh_store_log(st, "this system has no Skill Hub units or desktop files to remove; delete the application the way you installed it");
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
