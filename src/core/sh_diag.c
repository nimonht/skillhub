// See sh_diag.h.
#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE
#endif

#include "sh_diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh_fs.h"
#include "sh_git.h"
#include "sh_http.h"
#include "sh_path.h"
#include "sh_proc.h"
#include "sh_str.h"
#include "sh_sysinfo.h"
#include "sh_time.h"
#include "sh_utf.h"

#if defined(_WIN32)
#define PSAPI_VERSION 2  // kernel32 has its own copy of the memory counters: nothing more is loaded
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#else
#include <dirent.h>
#endif

#define SETTINGS_MAX (16 * 1024)

// ---- this process ----

#if defined(__linux__)
static long long status_kb(const char *text, const char *key) {
    const char *p = strstr(text, key);
    return p ? strtoll(p + strlen(key), NULL, 10) : -1;
}
#endif

bool sh_diag_memory_read(sh_diag_memory *m) {
    m->rss_kb = m->peak_kb = m->virtual_kb = m->threads = m->handles = -1;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX c;
    memset(&c, 0, sizeof c);
    c.cb = sizeof c;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&c, sizeof c)) return false;
    m->rss_kb = (long long)(c.WorkingSetSize / 1024);
    m->peak_kb = (long long)(c.PeakWorkingSetSize / 1024);
    m->virtual_kb = (long long)(c.PrivateUsage / 1024);
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles)) m->handles = (long long)handles;
    return true;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return false;
    m->rss_kb = (long long)(info.resident_size / 1024);
    m->peak_kb = (long long)(info.resident_size_max / 1024);
    m->virtual_kb = (long long)(info.virtual_size / 1024);
    thread_array_t threads = NULL;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &threads, &n) == KERN_SUCCESS) {
        m->threads = (long long)n;
        for (mach_msg_type_number_t i = 0; i < n; ++i) mach_port_deallocate(mach_task_self(), threads[i]);
        vm_deallocate(mach_task_self(), (vm_address_t)threads, n * sizeof *threads);
    }
    return true;
#else
    sh_buf status = SH_BUF_INIT;
    char *path = sh_strdup("/proc/self/status");
    const bool read = sh_read_file(path, 0, &status);
    free(path);
    if (!read) {
        sh_buf_free(&status);
        return false;
    }
    const char *text = sh_buf_str(&status);
    m->rss_kb = status_kb(text, "VmRSS:");
    m->peak_kb = status_kb(text, "VmHWM:");
    m->virtual_kb = status_kb(text, "VmSize:");
    m->threads = status_kb(text, "Threads:");
    sh_buf_free(&status);
    DIR *dir = opendir("/proc/self/fd");
    if (dir) {
        long long open = 0;
        while (readdir(dir)) ++open;
        closedir(dir);
        m->handles = open > 3 ? open - 3 : 0;  // ".", ".." and the one this listing holds
    }
    return true;
#endif
}

static char *megabytes(long long kb) { return sh_strfmt("%.1f MB", (double)kb / 1024.0); }

char *sh_diag_memory_text(const sh_diag_memory *m) {
    sh_buf b = SH_BUF_INIT;
    const char *sep = "";
    if (m->rss_kb >= 0) {
        char *rss = megabytes(m->rss_kb);
        sh_buf_printf(&b, "%s%s resident", sep, rss);
        free(rss);
        if (m->peak_kb >= 0) {
            char *peak = megabytes(m->peak_kb);
            sh_buf_printf(&b, " (peak %s)", peak);
            free(peak);
        }
        sep = ", ";
    }
    if (m->virtual_kb >= 0) {
        char *v = megabytes(m->virtual_kb);
        sh_buf_printf(&b, "%s%s %s", sep, v, 
#ifdef _WIN32
                      "private"
#else
                      "virtual"
#endif
        );
        free(v);
        sep = ", ";
    }
    if (m->threads >= 0) {
        sh_buf_printf(&b, "%s%lld threads", sep, m->threads);
        sep = ", ";
    }
    if (m->handles >= 0) sh_buf_printf(&b, "%s%lld %s", sep, m->handles,
#ifdef _WIN32
                                       "handles"
#else
                                       "open files"
#endif
    );
    if (!b.len) sh_buf_puts(&b, "not available on this system");
    return sh_buf_steal(&b);
}

// ---- the report ----

// Windows names one folder with either slash and in any case: the store's C:/Users/Me is also C:\Users\me in a variable
static bool same_path_char(char a, char b) {
#ifdef _WIN32
    if (a == '\\') a = '/';
    if (b == '\\') b = '/';
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
#endif
    return a == b;
}

// a letter of a file name (any byte of a UTF-8 one): the home folder is never found in the middle of a name
static bool name_char(char c) {
    const unsigned char u = (unsigned char)c;
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '.' || u == '_' || u == '-' || u >= 0x80;
}

// `home` at `p`, as a whole: not /home/u in /home/user, nor the second /home/u of /home/u/home/u
static bool home_at(const char *text, const char *p, const char *home, size_t n) {
    if (p > text && name_char(p[-1])) return false;
    for (size_t i = 0; i < n; ++i)
        if (!p[i] || !same_path_char(p[i], home[i])) return false;
    return !name_char(p[n]);
}

char *sh_diag_shorten(const char *text, const char *home) {
    const size_t n = home ? strlen(home) : 0;
    if (n < 2) return sh_strdup(text);  // "/" or "" would turn every slash into ~
    sh_buf b = SH_BUF_INIT;
    sh_buf_append(&b, "", 0);
    for (const char *p = text; *p;) {
        if (home_at(text, p, home, n)) {
            sh_buf_putc(&b, '~');
            p += n;
        } else {
            sh_buf_putc(&b, *p++);
        }
    }
    return sh_buf_steal(&b);
}

static void section(sh_buf *b, const char *title) { sh_buf_printf(b, "\n%s\n%.*s\n", title, (int)strlen(title), "------------------------------------------------"); }

static void line(sh_buf *b, const char *label, const char *value) { sh_buf_printf(b, "%-11s %s\n", label, value && *value ? value : "-"); }

// `home` is the store's own, which is what the paths in it start with
static void line_path(sh_buf *b, const char *label, const char *path, const char *home) {
    char *shown = sh_diag_shorten(path ? path : "", home);
    line(b, label, shown);
    free(shown);
}

// the settings file as it is, apart from the home folder
static void settings_part(sh_buf *b, const char *home) {
    char *config = sh_config_dir(), *path = sh_path_join(config, "settings.json");
    sh_buf raw = SH_BUF_INIT;
    if (!sh_read_file(path, SETTINGS_MAX + 1, &raw)) {
        sh_buf_puts(b, "no settings file\n");
    } else {
        const bool cut = raw.len > SETTINGS_MAX;
        if (cut) raw.len = SETTINGS_MAX, raw.data[raw.len] = '\0';
        char *shown = sh_diag_shorten(sh_buf_str(&raw), home);
        sh_buf_puts(b, shown);
        if (!b->len || b->data[b->len - 1] != '\n') sh_buf_putc(b, '\n');
        if (cut) sh_buf_puts(b, "... (cut)\n");
        free(shown);
    }
    sh_buf_free(&raw);
    free(path);
    free(config);
}

char *sh_diag_report(sh_store *st, const char *role) {
    sh_buf b = SH_BUF_INIT;
    char *when = sh_now_iso();
    sh_buf_printf(&b, "Skill Hub diagnostic report\n===========================\n");
    sh_buf_printf(&b, "Nothing here leaves this computer unless you send it. The home folder is written as ~; user and host names are left out.\n");
    section(&b, "Program");
    line(&b, "Generated", when);
    char *program = sh_strfmt("skillhub %s, %s, process %ld", SKILLHUB_VERSION, role, sh_process_id());
    line(&b, "Running", program);
    free(program);
    sh_diag_memory mem;
    char *used = sh_diag_memory_read(&mem) ? sh_diag_memory_text(&mem) : sh_strdup("not available on this system");
    line(&b, "Memory", used);
    free(used);

    section(&b, "Machine");
    sh_sysinfo info;
    sh_sysinfo_read(&info);
    line(&b, "System", info.os);
    line(&b, "Kernel", info.kernel);
    line(&b, "Shell", info.shell);
    line(&b, "Terminal", info.terminal);
    line(&b, "Uptime", info.uptime);
    sh_sysinfo_free(&info);

    section(&b, "Tools");
    const char *version_args[] = {"--version", NULL};
    sh_git_result g = sh_git_run(version_args, NULL, 5000);
    char *git = g.ok ? sh_trimmed(sh_buf_str(&g.out), g.out.len) : sh_strdup("not found");
    line(&b, "git", git);
    free(git);
    sh_git_result_free(&g);
    line(&b, "curl, wget", sh_http_available() ? "found" : "not found (Skill Hub cannot look for or download its own updates)");
    char *timer = sh_automation_state(st, true), *watch = sh_automation_state(st, false);
    char *autos = sh_strfmt("%s: daily update %s, stray watcher %s", sh_automation_mechanism(), timer, watch);
    line(&b, "Automation", autos);
    free(autos);
    free(timer);
    free(watch);

    section(&b, "Folders");
    line_path(&b, "Home", st->home, st->home);
    line_path(&b, "Skills", st->central, st->home);
    line_path(&b, "Backups", st->backup, st->home);
    line_path(&b, "Cache", st->cache, st->home);
    line_path(&b, "State", st->state, st->home);
    line_path(&b, "Lock file", st->lock_path, st->home);
    char *config = sh_config_dir();
    line_path(&b, "Config", config, st->home);
    free(config);

    section(&b, "Agents");
    for (size_t i = 0; i < st->n_agents; ++i) {
        char *dir = sh_diag_shorten(st->agents[i]->dir && *st->agents[i]->dir ? st->agents[i]->dir : "-", st->home);
        char *what = sh_strfmt("%-16s %s  (%s)", st->agents[i]->id, dir, st->agents[i]->detected_by ? st->agents[i]->detected_by : "");
        sh_buf_printf(&b, "%s\n", what);
        free(what);
        free(dir);
    }
    if (!st->n_agents) sh_buf_puts(&b, "none managed\n");

    section(&b, "Skills");
    sh_skills sk;
    sh_store_scan(st, &sk);
    size_t updates = 0, untracked = 0, errors = 0, broken = 0, parked = 0;
    for (size_t i = 0; i < sk.len; ++i) {
        updates += sk.items[i].update == SH_UPDATE_AVAILABLE;
        untracked += sk.items[i].update == SH_UPDATE_UNTRACKED;
        errors += sk.items[i].update == SH_UPDATE_ERROR;
        parked += sk.items[i].parked;
        for (size_t k = 0; k < sk.items[i].n_links; ++k) {
            const sh_link_state ls = sk.items[i].links[k];
            if (ls == SH_LINK_MISSING || ls == SH_LINK_COPY || ls == SH_LINK_FOREIGN) {
                ++broken;
                break;
            }
        }
    }
    char *counts = sh_strfmt("%zu skills: %zu with an update, %zu local, %zu with a failed check, %zu with link problems, %zu switched off", sk.len,
                             updates, untracked, errors, broken, parked);
    line(&b, "Count", counts);
    free(counts);
    sh_skills_free(&sk);

    section(&b, "Environment");
    static const char *const names[] = {"SKILLHUB_HOME", "SKILLHUB_CONFIG", "SKILLHUB_CACHE", "SKILLHUB_BACKEND", "SKILLHUB_GUI_BIN",
                                        "SKILLHUB_BIN_PATH", "SKILLHUB_NO_SYMLINKS", "SKILLHUB_AUTOMATION_DRYRUN", "SKILLHUB_UPDATE_REMOTE",
                                        "SKILLHUB_UPDATE_BASE", "DO_NOT_TRACK", "DISABLE_TELEMETRY"};
    bool any = false;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        char *v = sh_getenv(names[i]);
        if (v && *v) {
            char *shown = sh_diag_shorten(v, st->home);
            line(&b, names[i], shown);
            free(shown);
            any = true;
        }
        free(v);
    }
    if (!any) sh_buf_puts(&b, "none of the SKILLHUB_* variables is set\n");

    section(&b, "Settings (settings.json)");
    settings_part(&b, st->home);
    free(when);
    return sh_buf_steal(&b);
}
