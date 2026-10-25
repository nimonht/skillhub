#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE
#endif

#include "sh_path.h"

#include <stdlib.h>
#include <string.h>

#include "sh_str.h"

#ifdef _WIN32
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

#ifdef _WIN32
static bool is_drive(const char *p) {
    return ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':';
}
#endif

char *sh_path_clean(const char *in) {
    if (!in || !*in) return sh_strdup("");
    char *p = sh_strdup(in);
#ifdef _WIN32
    for (char *c = p; *c; ++c)
        if (*c == '\\') *c = '/';
#endif
    // the part that ".." can never climb above: "/", "//" (UNC on Windows), "C:/" or "C:"
    size_t root = 0;
#ifdef _WIN32
    if (is_drive(p)) root = p[2] == '/' ? 3 : 2;
    else if (p[0] == '/' && p[1] == '/') root = 2;
    else if (p[0] == '/') root = 1;
#else
    if (p[0] == '/') root = 1;
#endif
    const bool absolute = root > 0 && p[root - 1] == '/';

    sh_strv segs = SH_STRV_INIT;
    sh_strv parts = SH_STRV_INIT;
    sh_split(p + root, strlen(p + root), '/', true, &parts);
    for (size_t i = 0; i < parts.len; ++i) {
        const char *s = parts.items[i];
        if (strcmp(s, ".") == 0) continue;
        if (strcmp(s, "..") == 0) {
            if (segs.len > 0 && strcmp(segs.items[segs.len - 1], "..") != 0) {
                free(segs.items[--segs.len]);
                continue;
            }
            if (absolute) continue;  // nothing above the root
        }
        sh_strv_push(&segs, s);
    }
    sh_strv_free(&parts);

    sh_buf b = SH_BUF_INIT;
    sh_buf_append(&b, p, root);
    for (size_t i = 0; i < segs.len; ++i) {
        if (i) sh_buf_putc(&b, '/');
        sh_buf_puts(&b, segs.items[i]);
    }
    if (b.len == 0) sh_buf_putc(&b, '.');
    sh_strv_free(&segs);
    free(p);
    return sh_buf_steal(&b);
}

char *sh_path_join(const char *dir, const char *name) {
    if (!*dir) return sh_strdup(name);
    return sh_ends_with(dir, "/") ? sh_strfmt("%s%s", dir, name) : sh_strfmt("%s/%s", dir, name);
}

char *sh_path_join3(const char *a, const char *b, const char *c) {
    char *ab = sh_path_join(a, b);
    char *abc = sh_path_join(ab, c);
    free(ab);
    return abc;
}

char *sh_path_parent(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return sh_strdup("");
    if (slash == path) return sh_strdup("/");
#ifdef _WIN32
    if (slash == path + 2 && is_drive(path)) return sh_strndup(path, 3);
#endif
    return sh_strndup(path, (size_t)(slash - path));
}

const char *sh_path_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool sh_path_is_absolute(const char *path) {
#ifdef _WIN32
    if (is_drive(path) && (path[2] == '/' || path[2] == '\\')) return true;
    return path[0] == '/' || path[0] == '\\';
#else
    return path[0] == '/';
#endif
}

bool sh_path_is_root(const char *path) {
    if (strcmp(path, "/") == 0) return true;
#ifdef _WIN32
    if (is_drive(path) && (path[2] == '\0' || (path[2] == '/' && path[3] == '\0'))) return true;
    if (path[0] == '/' && path[1] == '/') {  // //server/share
        const char *s = strchr(path + 2, '/');
        return s && !strchr(s + 1, '/');
    }
#endif
    return false;
}

int sh_path_cmp(const char *a, const char *b) {
#ifdef _WIN32
    return _stricmp(a, b);
#else
    return strcmp(a, b);
#endif
}

bool sh_path_eq(const char *a, const char *b) { return sh_path_cmp(a, b) == 0; }

bool sh_path_within(const char *path, const char *dir) {
    const size_t n = strlen(dir);
    if (n == 0) return false;
#ifdef _WIN32
    if (_strnicmp(path, dir, n) != 0) return false;
#else
    if (strncmp(path, dir, n) != 0) return false;
#endif
    return path[n] == '\0' || path[n] == '/' || dir[n - 1] == '/';
}

char *sh_path_relative(const char *dir_in, const char *path_in) {
    char *dir = sh_path_clean(dir_in), *path = sh_path_clean(path_in);
    sh_strv d = SH_STRV_INIT, p = SH_STRV_INIT;
    sh_split(dir, strlen(dir), '/', true, &d);
    sh_split(path, strlen(path), '/', true, &p);
#ifdef _WIN32
    if (d.len && p.len && is_drive(d.items[0]) && _stricmp(d.items[0], p.items[0]) != 0) {  // another drive
        sh_strv_free(&d);
        sh_strv_free(&p);
        free(dir);
        return path;
    }
#endif
    size_t common = 0;
    while (common < d.len && common < p.len && sh_path_eq(d.items[common], p.items[common])) ++common;
    sh_buf b = SH_BUF_INIT;
    for (size_t i = common; i < d.len; ++i) sh_buf_puts(&b, b.len ? "/.." : "..");
    for (size_t i = common; i < p.len; ++i) {
        if (b.len) sh_buf_putc(&b, '/');
        sh_buf_puts(&b, p.items[i]);
    }
    sh_strv_free(&d);
    sh_strv_free(&p);
    free(dir);
    free(path);
    return sh_buf_steal(&b);
}

// ---- environment ----

char *sh_getenv(const char *name) {
#ifdef _WIN32
    wchar_t wname[128];
    if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 128)) return NULL;
    const DWORD n = GetEnvironmentVariableW(wname, NULL, 0);
    if (n == 0) return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? NULL : sh_strdup("");
    wchar_t *w = sh_xmalloc(n * sizeof *w);
    GetEnvironmentVariableW(wname, w, n);
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *out = sh_xmalloc((size_t)len);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, len, NULL, NULL);
    free(w);
    return out;
#else
    const char *v = getenv(name);
    return v ? sh_strdup(v) : NULL;
#endif
}

bool sh_env_is_set(const char *name) {
    char *v = sh_getenv(name);
    const bool set = v != NULL;
    free(v);
    return set;
}

// ---- folders ----

#ifdef _WIN32
static char *known_folder(REFKNOWNFOLDERID id) {
    PWSTR w = NULL;
    char *out = NULL;
    if (SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, NULL, &w) == S_OK) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
        out = sh_xmalloc((size_t)len);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, len, NULL, NULL);
    }
    CoTaskMemFree(w);
    if (!out) return sh_strdup("C:/");
    char *clean = sh_path_clean(out);
    free(out);
    return clean;
}
#endif

static char *system_home(void) {
#ifdef _WIN32
    return known_folder(&FOLDERID_Profile);  // like Qt: the profile folder, not the USERPROFILE variable
#else
    char *h = sh_getenv("HOME");
    if (h && *h) {
        char *clean = sh_path_clean(h);
        free(h);
        return clean;
    }
    free(h);
    const struct passwd *pw = getpwuid(getuid());
    return sh_path_clean(pw && pw->pw_dir ? pw->pw_dir : "/");
#endif
}

char *sh_home(void) {
    char *forced = sh_getenv("SKILLHUB_HOME");
    if (forced && *forced) {
        char *clean = sh_path_clean(forced);
        free(forced);
        return clean;
    }
    free(forced);
    return system_home();
}

#if !defined(_WIN32) && !defined(__APPLE__)
// $VAR when it is an absolute path, else ~/fallback (the XDG rule; Qt's QStandardPaths follows it)
static char *xdg(const char *var, const char *fallback) {
    char *v = sh_getenv(var);
    if (v && v[0] == '/') {
        char *clean = sh_path_clean(v);
        free(v);
        return clean;
    }
    free(v);
    char *home = system_home();
    char *out = sh_path_join(home, fallback);
    free(home);
    return out;
}
#endif

#ifdef __APPLE__
static char *account_home(void) {  // NSHomeDirectory: the account's home, whatever $HOME says
    const struct passwd *pw = getpwuid(getuid());
    return sh_path_clean(pw && pw->pw_dir ? pw->pw_dir : "/");
}
#endif

static char *overridden(const char *var) {
    if (!sh_env_is_set(var)) return NULL;
    char *v = sh_getenv(var);
    char *clean = sh_path_clean(v);  // set but empty stays "" (1.x behaviour, documented)
    free(v);
    return clean;
}

char *sh_config_dir(void) {
    char *o = overridden("SKILLHUB_CONFIG");
    if (o) return o;
#ifdef _WIN32
    char *base = known_folder(&FOLDERID_LocalAppData);
#elif defined(__APPLE__)
    char *home = account_home();
    char *base = sh_path_join(home, "Library/Preferences");
    free(home);
#else
    char *base = xdg("XDG_CONFIG_HOME", ".config");
#endif
    char *out = sh_path_join(base, "skillhub");
    free(base);
    return out;
}

char *sh_cache_dir(void) {
    char *o = overridden("SKILLHUB_CACHE");
    if (o) return o;
#ifdef _WIN32
    char *local = known_folder(&FOLDERID_LocalAppData);
    char *base = sh_path_join(local, "cache");
    free(local);
#elif defined(__APPLE__)
    char *home = account_home();
    char *base = sh_path_join(home, "Library/Caches");
    free(home);
#else
    char *base = xdg("XDG_CACHE_HOME", ".cache");
#endif
    char *out = sh_path_join(base, "skillhub");
    free(base);
    return out;
}

char *sh_state_dir(void) {
    char *o = overridden("SKILLHUB_STATE");
    if (o) return o;
#ifdef _WIN32
    char *local = known_folder(&FOLDERID_LocalAppData);
    char *out = sh_path_join(local, "skillhub/state");
    free(local);
    return out;
#elif defined(__APPLE__)
    char *home = account_home();
    char *out = sh_path_join(home, "Library/Application Support/skillhub");
    free(home);
    return out;
#else
    char *base = xdg("XDG_STATE_HOME", ".local/state");
    char *out = sh_path_join(base, "skillhub");
    free(base);
    return out;
#endif
}

char *sh_tilde(const char *path) {
    char *home = sh_home();
    char *out;
    if (sh_path_within(path, home) && *home) out = sh_strfmt("~%s", path + strlen(home));
    else out = sh_strdup(path);
    free(home);
    return out;
}
