#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE  // gethostname
#endif

#include "sh_sysinfo.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sh_fs.h"
#include "sh_path.h"
#include "sh_proc.h"
#include "sh_str.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <sys/sysinfo.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

static char *env_or(const char *name, const char *fallback) {
    char *v = sh_getenv(name);
    if (v && *v) return v;
    free(v);
    return sh_strdup(fallback);
}

static char *shown_uptime(long long seconds) {
    if (seconds <= 0) return sh_strdup("");
    const long long days = seconds / 86400, hours = seconds % 86400 / 3600, minutes = seconds % 3600 / 60;
    if (days) return sh_strfmt("%lld d %lld h", days, hours);
    if (hours) return sh_strfmt("%lld h %lld min", hours, minutes);
    return sh_strfmt("%lld min", minutes);
}

static char *terminal_name(void) {
    static const struct { const char *var, *name; } signs[] = {
        {"KITTY_WINDOW_ID", "kitty"}, {"GHOSTTY_RESOURCES_DIR", "ghostty"}, {"ALACRITTY_WINDOW_ID", "alacritty"}, {"WEZTERM_PANE", "wezterm"},
        {"KONSOLE_VERSION", "konsole"}, {"WT_SESSION", "Windows Terminal"}, {"SKILLHUB_TERMINAL", "Skill Hub"},
    };
    for (size_t i = sizeof signs / sizeof signs[0]; i-- > 0;)
        if (sh_env_is_set(signs[i].var)) return sh_strdup(signs[i].name);
    char *program = sh_getenv("TERM_PROGRAM");
    if (program && *program) return program;
    free(program);
    return env_or("TERM", "");
}

#ifndef _WIN32
static char *os_name(const char *machine) {
#ifdef __APPLE__
    const char *const argv[] = {"sw_vers", "-productVersion", NULL};
    const sh_proc_opts o = {NULL, NULL, NULL, 0, 3000, NULL, NULL};
    sh_proc_result r;
    sh_proc_run(argv, &o, &r);
    char *version = sh_trimmed(sh_buf_str(&r.out), r.out.len);
    sh_proc_result_free(&r);
    char *name = sh_strfmt("macOS %s %s", version, machine);
    free(version);
    return name;
#else
    sh_buf b = SH_BUF_INIT;
    char *pretty = NULL;
    if (sh_read_file("/etc/os-release", 0, &b)) {
        sh_strv lines = SH_STRV_INIT;
        sh_split(sh_buf_str(&b), b.len, '\n', true, &lines);
        for (size_t i = 0; i < lines.len && !pretty; ++i) {
            if (!sh_starts_with(lines.items[i], "PRETTY_NAME=")) continue;
            const char *v = lines.items[i] + 12;
            const size_t n = strlen(v);
            pretty = n >= 2 && v[0] == '"' ? sh_strndup(v + 1, n - 2) : sh_strdup(v);
        }
        sh_strv_free(&lines);
    }
    sh_buf_free(&b);
    char *name = sh_strfmt("%s %s", pretty ? pretty : "Linux", machine);
    free(pretty);
    return name;
#endif
}
#endif

void sh_sysinfo_read(sh_sysinfo *s) {
    memset(s, 0, sizeof *s);
#ifdef _WIN32
    s->user = env_or("USERNAME", "");
    s->host = env_or("COMPUTERNAME", "");
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    const char *arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64" : "x86_64";
    char product[128] = "Windows", build[32] = "";
    DWORD n = sizeof product;
    RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName", RRF_RT_REG_SZ, NULL, product, &n);
    n = sizeof build;
    RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentBuild", RRF_RT_REG_SZ, NULL, build, &n);
    s->os = sh_strfmt("%s %s", product, arch);
    s->kernel = sh_strfmt("NT build %s", build);
    s->uptime = shown_uptime((long long)(GetTickCount64() / 1000));
    s->shell = env_or("SKILLHUB_SHELL", sh_env_is_set("PSModulePath") ? "PowerShell" : "cmd");
#else
    struct utsname u;
    const bool have = uname(&u) == 0;
    char host[256] = "";
    gethostname(host, sizeof host - 1);
    s->host = sh_strdup(host);
    s->user = env_or("USER", "");
    s->os = os_name(have ? u.machine : "");
    s->kernel = have ? sh_strfmt("%s %s", u.sysname, u.release) : sh_strdup("");
    char *shell = env_or("SHELL", "");
    s->shell = sh_strdup(sh_path_name(shell));
    free(shell);
#if defined(__linux__)
    struct sysinfo info;
    s->uptime = shown_uptime(sysinfo(&info) == 0 ? (long long)info.uptime : 0);
#elif defined(__APPLE__)
    struct timeval boot;
    size_t len = sizeof boot;
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    s->uptime = shown_uptime(sysctl(mib, 2, &boot, &len, NULL, 0) == 0 ? (long long)(time(NULL) - boot.tv_sec) : 0);
#else
    s->uptime = sh_strdup("");
#endif
#endif
    s->terminal = terminal_name();
}

void sh_sysinfo_free(sh_sysinfo *s) {
    free(s->user);
    free(s->host);
    free(s->os);
    free(s->kernel);
    free(s->shell);
    free(s->terminal);
    free(s->uptime);
    memset(s, 0, sizeof *s);
}
