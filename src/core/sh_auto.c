// Automation: the daily update and the stray watcher. systemd user units on Linux (written exactly as 1.x wrote them),
// LaunchAgents on macOS, Task Scheduler on Windows. Every backend builds everywhere, and SKILLHUB_AUTOMATION_DRYRUN (systemd,
// launchd or schtasks; any other value: this system's own) writes the files and logs the commands instead of running them,
// so all three can be tested on any machine.
#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE  // setenv
#endif

#include <stdlib.h>
#include <string.h>

#include "sh_exe.h"
#include "sh_fs.h"
#include "sh_hash.h"
#include "sh_json.h"
#include "sh_path.h"
#include "sh_proc.h"
#include "sh_state.h"
#include "sh_store.h"
#include "sh_utf.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

extern const char sh_providers_json[];
extern const size_t sh_providers_json_len;

typedef enum { AUTO_NONE, AUTO_SYSTEMD, AUTO_LAUNCHD, AUTO_SCHTASKS } backend;

#define LAUNCHD_UPDATE "io.github.nimonht.skillhub.update"
#define LAUNCHD_ADOPT "io.github.nimonht.skillhub.adopt"
#define TASK_UPDATE "SkillHub\\Daily update"
#define TASK_WATCH "SkillHub\\Stray watcher"

static bool dry_run(void) {
    char *v = sh_getenv("SKILLHUB_AUTOMATION_DRYRUN");
    const bool yes = v && *v;
    free(v);
    return yes;
}

static backend backend_now(void) {
    char *v = sh_getenv("SKILLHUB_AUTOMATION_DRYRUN");
    backend b = AUTO_NONE;
    if (v && strcmp(v, "systemd") == 0) b = AUTO_SYSTEMD;
    else if (v && strcmp(v, "launchd") == 0) b = AUTO_LAUNCHD;
    else if (v && strcmp(v, "schtasks") == 0) b = AUTO_SCHTASKS;
    free(v);
    if (b != AUTO_NONE) return b;
#if defined(__linux__)
    return AUTO_SYSTEMD;
#elif defined(__APPLE__)
    return AUTO_LAUNCHD;
#elif defined(_WIN32)
    return AUTO_SCHTASKS;
#else
    return AUTO_NONE;
#endif
}

bool sh_automation_supported(void) { return backend_now() != AUTO_NONE; }

const char *sh_automation_mechanism(void) {
    switch (backend_now()) {
    case AUTO_SYSTEMD: return "systemd";
    case AUTO_LAUNCHD: return "launchd";
    case AUTO_SCHTASKS: return "Task Scheduler";
    default: return "none";
    }
}

const char *sh_automation_name(bool timer) {
    switch (backend_now()) {
    case AUTO_LAUNCHD: return timer ? LAUNCHD_UPDATE : LAUNCHD_ADOPT;
    case AUTO_SCHTASKS: return timer ? TASK_UPDATE : TASK_WATCH;
    default: return timer ? "skillhub-update.timer" : "skillhub-adopt.path";
    }
}

// runs a system tool; in a dry run the command is logged and counts as done
static bool tool(sh_store *st, const char *const *argv, char **out, int timeout_ms) {
    if (dry_run()) {
        sh_buf line = SH_BUF_INIT;
        for (size_t i = 0; argv[i]; ++i) sh_buf_printf(&line, "%s%s", i ? " " : "", argv[i]);
        sh_store_log(st, "would run: %s", sh_buf_str(&line));
        sh_buf_free(&line);
        if (out) *out = sh_strdup("");
        return true;
    }
    const sh_proc_opts o = {NULL, NULL, NULL, 0, timeout_ms ? timeout_ms : 60000, NULL, NULL};
    sh_proc_result r;
    const bool ok = sh_proc_run(argv, &o, &r);
    if (out) *out = sh_trimmed(sh_buf_str(&r.out), r.out.len);
    sh_proc_result_free(&r);
    return ok;
}

// the program a scheduled run starts: the command line, never the window
static char *program(const sh_store *st, backend b) {
    char self[4096];
    if (b == AUTO_SCHTASKS) {
        char dir[4096];
        return sh_exe_dir(dir, sizeof dir) ? sh_strfmt("%s/skillhubw.exe", dir) : sh_strdup("skillhubw.exe");
    }
    if (b == AUTO_SYSTEMD) {
        char *installed = sh_path_join(st->home, ".local/bin/skillhub");
        const bool have = sh_exists(installed);
        free(installed);
        if (have) return sh_strdup("%h/.local/bin/skillhub");
    }
    return sh_exe_path(self, sizeof self) ? sh_strdup(self) : sh_strdup("skillhub");
}

static void xml_put(sh_buf *b, const char *s) {
    for (; *s; ++s) {
        if (*s == '&') sh_buf_puts(b, "&amp;");
        else if (*s == '<') sh_buf_puts(b, "&lt;");
        else if (*s == '>') sh_buf_puts(b, "&gt;");
        else sh_buf_putc(b, *s);
    }
}

// a time of day that stays the same on this machine, and differs between machines, so not everybody hits a server at once
static void pick_time(const sh_store *st, int *hour, int *minute) {
    sh_sha1 h;
    sh_sha1_init(&h);
    sh_sha1_update(&h, st->home, strlen(st->home));
    unsigned char d[20];
    sh_sha1_final(&h, d);
    *hour = 9 + d[0] % 12;
    *minute = d[1] % 60;
}

// ---- systemd ----

static char *unit_dir(const sh_store *st) { return sh_path_join(st->home, ".config/systemd/user"); }

static char *path_unit_text(const sh_store *st) {
    sh_strv dirs = SH_STRV_INIT;
    sh_store_watch_dirs(st, &dirs);
    sh_buf paths = SH_BUF_INIT;
    for (size_t i = 0; i < dirs.len; ++i) sh_buf_printf(&paths, "PathChanged=%s\n", dirs.items[i]);
    sh_strv_free(&dirs);
    char *text = sh_strfmt("[Unit]\nDescription=Watch agent skill dirs for stray skills\n\n"
                           "[Path]\n%s\n"
                           "[Install]\nWantedBy=default.target\n", sh_buf_str(&paths));
    sh_buf_free(&paths);
    return text;
}

static bool systemctl(sh_store *st, const char *a, const char *b, const char *c) {
    const char *argv[] = {"systemctl", "--user", a, b, c, NULL};
    return tool(st, argv, NULL, 0);
}

static bool systemd_install(sh_store *st, bool timer, bool watcher, char **err) {
    char *exe = program(st, AUTO_SYSTEMD);
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
        char *service = sh_strfmt("[Unit]\nDescription=Pull stray agent skills into ~/.agents/skills\n"
                                  "StartLimitIntervalSec=0\n\n"
                                  "[Service]\nType=oneshot\nExecStart=%s adopt --notify\n", exe);
        char *path_unit = path_unit_text(st);
        char *f1 = sh_path_join(d, "skillhub-adopt.service"), *f2 = sh_path_join(d, "skillhub-adopt.path");
        ok = sh_write_atomic(f1, service, strlen(service)) && ok;
        ok = sh_write_atomic(f2, path_unit, strlen(path_unit)) && ok;
        free(f1);
        free(f2);
        free(service);
        free(path_unit);
    }
    if (!ok) {
        if (err) *err = sh_strfmt("cannot write unit files in %s", d);
        free(d);
        free(exe);
        return false;
    }
    systemctl(st, "daemon-reload", NULL, NULL);
    if (timer) ok = systemctl(st, "enable", "--now", "skillhub-update.timer") && ok;
    if (watcher) ok = systemctl(st, "enable", "--now", "skillhub-adopt.path") && ok;
    if (!ok && err) *err = sh_strdup("systemctl --user enable failed");
    free(d);
    free(exe);
    return ok;
}

static void systemd_remove(sh_store *st, bool timer, bool watcher) {
    char *d = unit_dir(st);
    if (timer) {
        systemctl(st, "disable", "--now", "skillhub-update.timer");
        char *f1 = sh_path_join(d, "skillhub-update.timer"), *f2 = sh_path_join(d, "skillhub-update.service");
        sh_remove_file(f1);
        sh_remove_file(f2);
        free(f1);
        free(f2);
    }
    if (watcher) {
        systemctl(st, "disable", "--now", "skillhub-adopt.path");
        char *f1 = sh_path_join(d, "skillhub-adopt.path"), *f2 = sh_path_join(d, "skillhub-adopt.service");
        sh_remove_file(f1);
        sh_remove_file(f2);
        free(f1);
        free(f2);
    }
    systemctl(st, "daemon-reload", NULL, NULL);
    free(d);
}

static char *systemd_state(const char *unit) {
    if (dry_run()) return sh_strdup("not installed");
    const char *argv[] = {"systemctl", "--user", "is-enabled", unit, NULL};
    const sh_proc_opts o = {NULL, NULL, NULL, 0, 8000, NULL, NULL};
    sh_proc_result r;
    sh_proc_run(argv, &o, &r);
    char *out = sh_trimmed(sh_buf_str(&r.out), r.out.len);
    sh_proc_result_free(&r);
    if (!*out) {
        free(out);
        return sh_strdup("not installed");
    }
    return out;
}

// ---- launchd ----

static char *plist_path(const sh_store *st, const char *label) {
    char *dir = sh_path_join(st->home, "Library/LaunchAgents");
    char *p = sh_strfmt("%s/%s.plist", dir, label);
    free(dir);
    return p;
}

static long user_id(void) {
#ifdef _WIN32
    return 0;
#else
    return (long)getuid();
#endif
}

static void plist_strings(sh_buf *b, const char *key, const char *const *items, size_t n) {
    sh_buf_printf(b, "    <key>%s</key>\n    <array>\n", key);
    for (size_t i = 0; i < n; ++i) {
        sh_buf_puts(b, "        <string>");
        xml_put(b, items[i]);
        sh_buf_puts(b, "</string>\n");
    }
    sh_buf_puts(b, "    </array>\n");
}

static void plist_string(sh_buf *b, const char *key, const char *value) {
    sh_buf_printf(b, "    <key>%s</key>\n    <string>", key);
    xml_put(b, value);
    sh_buf_puts(b, "</string>\n");
}

static char *plist_text(const sh_store *st, bool timer) {
    char *exe = program(st, AUTO_LAUNCHD);
    char *logs = sh_path_join(st->home, "Library/Logs/skillhub");
    char *log = sh_strfmt("%s/%s.log", logs, timer ? "update" : "adopt");
    sh_buf b = SH_BUF_INIT;
    sh_buf_puts(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                    "<plist version=\"1.0\">\n<dict>\n");
    plist_string(&b, "Label", timer ? LAUNCHD_UPDATE : LAUNCHD_ADOPT);
    if (timer) {
        const char *const args[] = {exe, "update", "--all", "--auto", "--notify"};
        plist_strings(&b, "ProgramArguments", args, 5);
        int hour, minute;
        pick_time(st, &hour, &minute);
        sh_buf_printf(&b, "    <key>StartCalendarInterval</key>\n    <dict>\n        <key>Hour</key>\n        <integer>%d</integer>\n"
                          "        <key>Minute</key>\n        <integer>%d</integer>\n    </dict>\n"
                          "    <key>Nice</key>\n    <integer>10</integer>\n", hour, minute);
    } else {
        const char *const args[] = {exe, "adopt", "--notify"};
        plist_strings(&b, "ProgramArguments", args, 3);
        sh_strv dirs = SH_STRV_INIT;
        sh_store_watch_dirs(st, &dirs);
        plist_strings(&b, "WatchPaths", (const char *const *)dirs.items, dirs.len);
        sh_strv_free(&dirs);
        sh_buf_puts(&b, "    <key>ThrottleInterval</key>\n    <integer>10</integer>\n");
    }
    plist_string(&b, "StandardOutPath", log);
    plist_string(&b, "StandardErrorPath", log);
    sh_buf_puts(&b, "</dict>\n</plist>\n");
    char *text = sh_strdup(sh_buf_str(&b));
    sh_buf_free(&b);
    free(log);
    free(logs);
    free(exe);
    return text;
}

static bool launchctl(sh_store *st, const char *verb, const char *target, const char *plist) {
    const char *argv[] = {"launchctl", verb, target, plist, NULL};
    return tool(st, argv, NULL, 0);
}

static bool launchd_one(sh_store *st, bool timer, char **err) {
    const char *label = timer ? LAUNCHD_UPDATE : LAUNCHD_ADOPT;
    char *text = plist_text(st, timer), *path = plist_path(st, label);
    char *logs = sh_path_join(st->home, "Library/Logs/skillhub");
    sh_mkpath(logs);
    free(logs);
    bool ok = sh_write_atomic(path, text, strlen(text));
    if (ok) {
        char *domain = sh_strfmt("gui/%ld", user_id()), *service = sh_strfmt("gui/%ld/%s", user_id(), label);
        launchctl(st, "bootout", service, NULL);  // running already: it reads the new file only when loaded again
        ok = launchctl(st, "bootstrap", domain, path);
        if (!ok && err) *err = sh_strfmt("launchctl could not load %s", label);
        free(service);
        free(domain);
    } else if (err) {
        *err = sh_strfmt("cannot write %s", path);
    }
    free(path);
    free(text);
    return ok;
}

static void launchd_remove(sh_store *st, bool timer) {
    const char *label = timer ? LAUNCHD_UPDATE : LAUNCHD_ADOPT;
    char *service = sh_strfmt("gui/%ld/%s", user_id(), label), *path = plist_path(st, label);
    launchctl(st, "bootout", service, NULL);
    sh_remove_file(path);
    free(path);
    free(service);
}

static char *launchd_state(const sh_store *st, bool timer) {
    const char *label = timer ? LAUNCHD_UPDATE : LAUNCHD_ADOPT;
    char *path = plist_path(st, label);
    const bool have = sh_exists(path);
    free(path);
    if (!have) return sh_strdup("not installed");
    if (dry_run()) return sh_strdup("enabled");
    char *service = sh_strfmt("gui/%ld/%s", user_id(), label);
    const char *argv[] = {"launchctl", "print", service, NULL};
    const sh_proc_opts o = {NULL, NULL, NULL, 0, 8000, NULL, NULL};
    sh_proc_result r;
    const bool loaded = sh_proc_run(argv, &o, &r);
    sh_proc_result_free(&r);
    free(service);
    return sh_strdup(loaded ? "enabled" : "disabled");
}

// ---- Task Scheduler ----

// The command line's flags are localized, so tasks are imported as XML, in UTF-16 as schtasks expects it.
static char *task_text(const sh_store *st, bool timer) {
    char *exe = program(st, AUTO_SCHTASKS);
    for (char *c = exe; *c; ++c)
        if (*c == '/') *c = '\\';
    sh_buf b = SH_BUF_INIT;
    sh_buf_puts(&b, "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n<Task version=\"1.4\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n");
    sh_buf_printf(&b, "  <RegistrationInfo><Description>%s</Description></RegistrationInfo>\n  <Triggers>\n",
                  timer ? "Update shared agent skills" : "Pull stray agent skills into the shared folder");
    if (timer) {
        int hour, minute;
        pick_time(st, &hour, &minute);
        sh_buf_printf(&b, "    <CalendarTrigger><StartBoundary>2026-01-01T%02d:%02d:00</StartBoundary><RandomDelay>PT30M</RandomDelay>"
                          "<ScheduleByDay><DaysInterval>1</DaysInterval></ScheduleByDay></CalendarTrigger>\n", hour, minute);
    } else {
        sh_buf_puts(&b, "    <LogonTrigger><Enabled>true</Enabled></LogonTrigger>\n");
    }
    sh_buf_puts(&b, "  </Triggers>\n  <Principals><Principal id=\"Author\"><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>\n"
                    "  <Settings>\n    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
                    "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
                    "    <StartWhenAvailable>true</StartWhenAvailable>\n");
    if (timer) sh_buf_puts(&b, "    <RunOnlyIfNetworkAvailable>true</RunOnlyIfNetworkAvailable>\n    <ExecutionTimeLimit>PT1H</ExecutionTimeLimit>\n");
    else sh_buf_puts(&b, "    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n    <RestartOnFailure><Interval>PT1M</Interval><Count>3</Count></RestartOnFailure>\n");
    sh_buf_puts(&b, "  </Settings>\n  <Actions Context=\"Author\"><Exec><Command>");
    xml_put(&b, exe);
    sh_buf_printf(&b, "</Command><Arguments>%s</Arguments></Exec></Actions>\n</Task>\n", timer ? "update --all --auto --notify" : "watch --run");
    char *text = sh_strdup(sh_buf_str(&b));
    sh_buf_free(&b);
    free(exe);
    return text;
}

static bool write_utf16(const char *path, const char *text) {
    sh_buf out = SH_BUF_INIT;
    sh_buf_append(&out, "\xff\xfe", 2);
    const char *p = text, *end = text + strlen(text);
    while (p < end) {
        uint32_t cp = sh_utf8_next(&p, end);
        unsigned char u[4];
        size_t n = 2;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            const uint32_t hi = 0xD800 + (cp >> 10), lo = 0xDC00 + (cp & 0x3FF);
            u[0] = (unsigned char)hi; u[1] = (unsigned char)(hi >> 8); u[2] = (unsigned char)lo; u[3] = (unsigned char)(lo >> 8);
            n = 4;
        } else {
            u[0] = (unsigned char)cp; u[1] = (unsigned char)(cp >> 8);
        }
        sh_buf_append(&out, (const char *)u, n);
    }
    const bool ok = sh_write_atomic(path, out.data, out.len);
    sh_buf_free(&out);
    return ok;
}

// the task's file stays in the cache only in a dry run, for the tests to read
static char *task_file(const sh_store *st, bool timer) { return sh_strfmt("%s/automation/%s.xml", st->cache, timer ? "update" : "watcher"); }

static bool schtasks_one(sh_store *st, bool timer, char **err) {
    const char *name = timer ? TASK_UPDATE : TASK_WATCH;
    char *text = task_text(st, timer), *path = task_file(st, timer);
    bool ok = write_utf16(path, text);
    if (ok) {
        char *out = NULL;
        const char *create[] = {"schtasks", "/Create", "/XML", path, "/TN", name, "/F", NULL};
        ok = tool(st, create, &out, 0);
        free(out);
        if (ok && !timer) {  // it runs from now on, not only from the next logon
            const char *start[] = {"schtasks", "/Run", "/TN", name, NULL};
            tool(st, start, NULL, 0);
        }
        if (!ok && err) *err = sh_strfmt("schtasks could not create the task %s", name);
    } else if (err) {
        *err = sh_strfmt("cannot write %s", path);
    }
    if (!dry_run()) sh_remove_file(path);
    free(path);
    free(text);
    return ok;
}

static void schtasks_remove(sh_store *st, bool timer) {
    const char *name = timer ? TASK_UPDATE : TASK_WATCH;
    if (!timer) {
        const char *stop[] = {"schtasks", "/End", "/TN", name, NULL};
        tool(st, stop, NULL, 0);
    }
    const char *del[] = {"schtasks", "/Delete", "/TN", name, "/F", NULL};
    tool(st, del, NULL, 0);
    char *path = task_file(st, timer);
    sh_remove_file(path);
    free(path);
}

static char *schtasks_state(const sh_store *st, bool timer) {
    if (dry_run()) {
        char *path = task_file(st, timer);
        const bool have = sh_exists(path);
        free(path);
        return sh_strdup(have ? "enabled" : "not installed");
    }
    const char *argv[] = {"schtasks", "/Query", "/TN", timer ? TASK_UPDATE : TASK_WATCH, NULL};
    const sh_proc_opts o = {NULL, NULL, NULL, 0, 8000, NULL, NULL};
    sh_proc_result r;
    const bool have = sh_proc_run(argv, &o, &r);
    sh_proc_result_free(&r);
    return sh_strdup(have ? "enabled" : "not installed");
}

// ---- the environment of scheduled runs ----

static void keep_var(yyjson_mut_doc *m, yyjson_mut_val *env, const char *name, bool *changed) {
    char *value = sh_getenv(name);
    if (value && *value) {
        sh_json_set_str(m, env, name, value);
        *changed = true;
    }
    free(value);
}

// Scheduled runs lack the login environment, so a variable like CLAUDE_CONFIG_DIR would be missing and an agent's folder would
// resolve differently, and without XDG_STATE_HOME so would the lock file npx uses. The variables the catalog refers to and
// the ones paths depend on are recorded when automation is switched on.
static void record_env(void) {
    yyjson_mut_doc *m = sh_state_load(NULL);
    if (!m) return;
    yyjson_mut_val *env = sh_json_get_or_add_obj(m, yyjson_mut_doc_get_root(m), "automationEnv");
    bool changed = false;
    const char *p = sh_providers_json, *end = sh_providers_json + sh_providers_json_len;
    while (p < end && (p = strstr(p, "${"))) {
        p += 2;
        const size_t n = strspn(p, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
        char *name = sh_strndup(p, n);
        if (n) keep_var(m, env, name, &changed);
        free(name);
        p += n;
    }
    static const char *const paths[] = {"XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME"};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; ++i) keep_var(m, env, paths[i], &changed);
    if (changed) sh_state_save(m);
    yyjson_mut_doc_free(m);
}

void sh_automation_apply_env(void) {
    yyjson_mut_doc *m = sh_state_load(NULL);
    yyjson_mut_val *env = m ? yyjson_mut_obj_get(yyjson_mut_doc_get_root(m), "automationEnv") : NULL;
    size_t i, n;
    yyjson_mut_val *key, *val;
    if (yyjson_mut_is_obj(env)) {
        yyjson_mut_obj_foreach(env, i, n, key, val) {
            const char *name = yyjson_mut_get_str(key);
            if (sh_env_is_set(name) || !yyjson_mut_is_str(val)) continue;
#ifdef _WIN32
            _putenv_s(name, yyjson_mut_get_str(val));
#else
            setenv(name, yyjson_mut_get_str(val), 0);
#endif
        }
    }
    yyjson_mut_doc_free(m);
}

// ---- the same for every system ----

char *sh_automation_state(const sh_store *st, bool timer) {
    switch (backend_now()) {
    case AUTO_SYSTEMD: return systemd_state(timer ? "skillhub-update.timer" : "skillhub-adopt.path");
    case AUTO_LAUNCHD: return launchd_state(st, timer);
    case AUTO_SCHTASKS: return schtasks_state(st, timer);
    default: return sh_strdup("not available");
    }
}

bool sh_store_install_units(sh_store *st, bool timer, bool watcher, char **err) {
    const backend b = backend_now();
    if (b == AUTO_NONE) {
        if (err) *err = sh_strdup("daily updates and the stray watcher are not available on this system");
        return false;
    }
    if (watcher) {
        sh_strv dirs = SH_STRV_INIT;
        sh_store_watch_dirs(st, &dirs);
        const size_t n = dirs.len;
        sh_strv_free(&dirs);
        if (n == 0) {
            if (err) *err = sh_strdup("no coding agents are managed yet, so there is nothing to watch");
            return false;
        }
    }
    record_env();
    if (b == AUTO_SYSTEMD) return systemd_install(st, timer, watcher, err);
    bool ok = true;
    if (timer) ok = (b == AUTO_LAUNCHD ? launchd_one(st, true, err) : schtasks_one(st, true, err)) && ok;
    if (watcher) ok = (b == AUTO_LAUNCHD ? launchd_one(st, false, err) : schtasks_one(st, false, err)) && ok;
    return ok;
}

bool sh_store_remove_units(sh_store *st, bool timer, bool watcher, char **err) {
    (void)err;
    const backend b = backend_now();
    if (b == AUTO_SYSTEMD) {
        systemd_remove(st, timer, watcher);
    } else if (b == AUTO_LAUNCHD) {
        if (timer) launchd_remove(st, true);
        if (watcher) launchd_remove(st, false);
    } else if (b == AUTO_SCHTASKS) {
        if (timer) schtasks_remove(st, true);
        if (watcher) schtasks_remove(st, false);
    }
    return true;
}

bool sh_store_watcher_stale(const sh_store *st) {
    const backend b = backend_now();
    if (b == AUTO_SCHTASKS || b == AUTO_NONE) return false;  // the resident watcher reads the list again by itself
    char *path = b == AUTO_SYSTEMD ? sh_path_join(st->home, ".config/systemd/user/skillhub-adopt.path") : plist_path(st, LAUNCHD_ADOPT);
    sh_buf have = SH_BUF_INIT;
    const bool installed = sh_read_file(path, 0, &have);
    free(path);
    bool stale = false;
    if (installed) {
        char *want = b == AUTO_SYSTEMD ? path_unit_text(st) : plist_text(st, false);
        stale = strcmp(want, sh_buf_str(&have)) != 0;
        free(want);
    }
    sh_buf_free(&have);
    return stale;
}

bool sh_store_sync_watcher(sh_store *st) {
    if (!sh_store_watcher_stale(st)) return false;
    char *err = NULL;
    const bool ok = sh_store_install_units(st, false, true, &err);
    if (ok) sh_store_log(st, "updated the stray watcher for the agents on this machine");
    else sh_store_log(st, "FAILED to update the stray watcher: %s", err ? err : "");
    free(err);
    return ok;
}
