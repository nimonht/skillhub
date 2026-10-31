#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE  // getcwd
#endif

#include "sh_origin.h"

#include <stdlib.h>
#include <string.h>

#include "sh_hash.h"
#include "sh_path.h"
#include "sh_str.h"
#include "sh_utf.h"

#ifdef _WIN32
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

static const char *const TYPE_NAMES[] = {"github", "gitlab", "git", "local"};

const char *sh_origin_type_name(sh_origin_type t) { return TYPE_NAMES[t]; }

bool sh_origin_type_from_name(const char *name, sh_origin_type *t) {
    for (int i = 0; i < 4; ++i) {
        if (name && strcmp(name, TYPE_NAMES[i]) == 0) {
            *t = (sh_origin_type)i;
            return true;
        }
    }
    return false;
}

bool sh_origin_ref_ok(const char *ref) {
    if (ref[0] == '-' || strstr(ref, "..")) return false;
    for (const char *c = ref; *c; ++c)
        if ((unsigned char)*c <= ' ' || *c == 0x7f || strchr("~^:?*[\\", *c)) return false;
    return true;
}

void sh_origin_free(sh_origin *o) {
    free(o->source);
    free(o->url);
    free(o->ref);
    free(o->subpath);
    free(o->host);
    memset(o, 0, sizeof *o);
}

static bool word_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

static void lower(char *s) {
    for (; *s; ++s)
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

static char *strip_git(const char *path) {  // "group/repo.git/" -> "group/repo"
    size_t n = strlen(path);
    while (n > 0 && path[n - 1] == '/') --n;
    if (n >= 4 && strncmp(path + n - 4, ".git", 4) == 0) n -= 4;
    return sh_strndup(path, n);
}

static void set(sh_origin *o, sh_origin_type type, char *source, char *url, const char *host) {
    o->type = type;
    o->source = source;
    o->url = url;
    o->host = sh_strdup(host);
    lower(o->host);
}

static bool is_local(const char *s) {
    if (s[0] == '/' || s[0] == '\\' || strcmp(s, "~") == 0 || sh_starts_with(s, "~/") || strcmp(s, ".") == 0 || strcmp(s, "..") == 0 ||
        sh_starts_with(s, "./") || sh_starts_with(s, "../"))
        return true;
    return ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) && s[1] == ':' && (s[2] == '/' || s[2] == '\\');
}

static void parse_local(const char *s, sh_origin *o) {
    char *path = NULL;
    if (s[0] == '~') {
        char *home = sh_home();
        path = sh_strfmt("%s%s", home, s + 1);
        free(home);
    } else if (sh_path_is_absolute(s)) {
        path = sh_strdup(s);
    } else {
        char cwd[4096];
        path = sh_path_join(getcwd(cwd, sizeof cwd) ? cwd : ".", s);
    }
    for (char *c = path; *c; ++c)
        if (*c == '\\') *c = '/';
    char *clean = sh_path_clean(path);
    free(path);
    set(o, SH_ORIGIN_LOCAL, sh_strdup(clean), clean, "");
}

// owner/repo, then an optional subpath; owner and repo are word characters only
static bool parse_shorthand(const char *s, sh_origin *o) {
    size_t on = 0;
    while (word_char(s[on])) ++on;
    if (on == 0 || s[on] != '/') return false;
    const char *r = s + on + 1;
    size_t rn = 0;
    while (word_char(r[rn])) ++rn;
    if (rn == 0 || (r[rn] && r[rn] != '/')) return false;
    char *name = sh_strndup(r, rn);  // ".git" is not part of the name
    char *bare = strip_git(name);
    free(name);
    char *slug = sh_strfmt("%.*s/%s", (int)on, s, bare);
    set(o, SH_ORIGIN_GITHUB, slug, sh_strfmt("https://github.com/%s.git", slug), "github.com");
    free(bare);
    if (r[rn] == '/') {
        free(o->subpath);
        o->subpath = strip_git(r + rn + 1);
    }
    return true;
}

// "owner/repo/tree/<ref>/<subpath>" style paths: everything before `marker` is the repository
static void split_tree(const char *path, const char *marker, char **repo, char **ref, char **subpath) {
    const char *t = strstr(path, marker);
    if (!t) {
        *repo = strip_git(path);
        return;
    }
    char *before = sh_strndup(path, (size_t)(t - path));
    *repo = strip_git(before);
    free(before);
    const char *rest = t + strlen(marker);
    const char *slash = strchr(rest, '/');
    free(*ref);
    *ref = slash ? sh_strndup(rest, (size_t)(slash - rest)) : sh_strdup(rest);
    if (slash) {
        free(*subpath);
        *subpath = strip_git(slash + 1);
    }
}

// what git talks to a server with; never a helper such as ext:: or fd::
static bool known_scheme(const char *s, size_t n) {
    static const char *const schemes[] = {"https", "http", "ssh", "git", "git+ssh"};
    for (size_t i = 0; i < sizeof schemes / sizeof schemes[0]; ++i)
        if (strlen(schemes[i]) == n && strncmp(s, schemes[i], n) == 0) return true;
    return false;
}

// a host becomes a folder of the cache, and git must never read it as an option
static bool host_ok(const char *h) {
    if (!*h || *h == '-') return false;
    bool only_dots = true;
    for (; *h; ++h) {
        if (!word_char(*h)) return false;
        only_dots = only_dots && *h == '.';
    }
    return !only_dots;
}

// a URL or git@host:path: the host decides between GitHub, GitLab and plain git
static bool parse_remote(const char *s, sh_origin *o) {
    const char *scheme_end = strstr(s, "://");
    const char *host_start, *path_start;
    char *user_host = NULL;
    if (scheme_end) {
        if (!known_scheme(s, (size_t)(scheme_end - s))) return false;
        host_start = scheme_end + 3;
        const char *slash = strchr(host_start, '/');
        if (!slash || slash == host_start) return false;
        user_host = sh_strndup(host_start, (size_t)(slash - host_start));
        path_start = slash + 1;
    } else {  // scp-like: [user@]host:path
        const char *colon = strchr(s, ':'), *slash = strchr(s, '/');
        if (!colon || (slash && slash < colon) || colon == s || colon[1] == ':') return false;  // ext::command is no host
        user_host = sh_strndup(s, (size_t)(colon - s));
        path_start = colon + 1;
    }
    char *host_port = sh_strdup(strchr(user_host, '@') ? strchr(user_host, '@') + 1 : user_host);  // a self-hosted server may use a port
    char *host = sh_strdup(host_port);
    char *port = strchr(host, ':');
    if (port) *port = '\0';
    lower(host);
    const bool usable = *user_host != '-' && host_ok(host);
    free(user_host);
    if (!usable) {
        free(host);
        free(host_port);
        return false;
    }
    const bool https = sh_starts_with(s, "https://");
    const bool web = https || sh_starts_with(s, "http://");
    char *repo = NULL, *ref = sh_strdup(""), *subpath = sh_strdup("");
    if (strcmp(host, "github.com") == 0 || strcmp(host, "www.github.com") == 0) {
        split_tree(path_start, "/tree/", &repo, &ref, &subpath);
        size_t on = 0;
        while (word_char(repo[on])) ++on;
        const bool owner_repo = on > 0 && repo[on] == '/' && repo[on + 1] && !strchr(repo + on + 1, '/');
        if (owner_repo) {
            set(o, SH_ORIGIN_GITHUB, sh_strdup(repo), web ? sh_strfmt("https://github.com/%s.git", repo) : sh_strdup(s), "github.com");
        } else {
            set(o, SH_ORIGIN_GIT, sh_strdup(s), sh_strdup(s), host);
        }
    } else if (strstr(path_start, "/-/tree/") || strcmp(host, "gitlab.com") == 0 || strstr(host, "gitlab")) {
        split_tree(path_start, "/-/tree/", &repo, &ref, &subpath);
        char *url = web ? sh_strfmt("%s://%s/%s.git", https ? "https" : "http", host_port, repo) : sh_strdup(s);
        set(o, SH_ORIGIN_GITLAB, sh_strdup(url), url, host);
    } else {
        set(o, SH_ORIGIN_GIT, sh_strdup(s), sh_strdup(s), host);
    }
    free(o->ref);
    free(o->subpath);
    o->ref = ref;
    o->subpath = subpath;
    free(repo);
    free(host);
    free(host_port);
    return true;
}

bool sh_origin_parse(const char *input, sh_origin *out, char **err) {
    memset(out, 0, sizeof *out);
    out->ref = sh_strdup("");
    out->subpath = sh_strdup("");
    char *s = sh_trimmed(input, strlen(input));
    bool ok = *s && s[0] != '-';
    if (ok && is_local(s)) {
        parse_local(s, out);
        free(s);
        return true;
    }
    char *frag = ok ? strrchr(s, '#') : NULL;  // owner/repo#ref, URL#ref
    char *ref = NULL;
    if (frag) {
        ref = sh_strdup(frag + 1);
        *frag = '\0';
    }
    if (!ok) {
    } else if (sh_starts_with(s, "github:")) {
        ok = parse_shorthand(s + 7, out);
    } else if (sh_starts_with(s, "gitlab:")) {
        char *path = strip_git(s + 7);
        ok = *path != '\0';
        if (ok) {
            char *url = sh_strfmt("https://gitlab.com/%s.git", path);
            set(out, SH_ORIGIN_GITLAB, sh_strdup(url), url, "gitlab.com");
        }
        free(path);
    } else if (strstr(s, "://") || (strchr(s, '@') && strchr(s, ':'))) {
        ok = parse_remote(s, out);
    } else {
        ok = parse_shorthand(s, out);
    }
    if (ok && ref && *ref) {
        free(out->ref);
        out->ref = ref;
        ref = NULL;
    }
    if (ok) ok = sh_origin_ref_ok(out->ref);  // also a ref taken from a /tree/<ref>/ URL
    free(ref);
    free(s);
    if (!ok) {
        sh_origin_free(out);
        if (err) *err = sh_strdup("expected owner/repo, a GitLab or git URL, or a local folder");
    }
    return ok;
}

// lower-case scheme and host, no trailing "/" or ".git"; git@host:path becomes ssh://git@host/path
static char *normalized_url(const char *url) {
    char *u;
    if (!strstr(url, "://") && strchr(url, '@') && strchr(url, ':')) {
        const char *colon = strchr(url, ':');
        u = sh_strfmt("ssh://%.*s/%s", (int)(colon - url), url, colon + 1);
    } else {
        u = sh_strdup(url);
    }
    char *scheme_end = strstr(u, "://");
    char *path = scheme_end ? strchr(scheme_end + 3, '/') : NULL;
    for (char *c = u; *c && (!path || c < path); ++c)
        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
    char *clean = strip_git(u);
    free(u);
    return clean;
}

char *sh_origin_cache_dir(const sh_origin *o, const char *cache) {
    if (o->type == SH_ORIGIN_GITHUB && !*o->ref && strstr(o->url, "https://github.com/") == o->url) {
        sh_buf k = SH_BUF_INIT;  // the key older versions used: owner__repo
        for (const char *c = o->source; *c; ++c) {
            if (*c == '/') sh_buf_puts(&k, "__");
            else sh_buf_putc(&k, *c);
        }
        char *dir = sh_strfmt("%s/repos/%s", cache, sh_buf_str(&k));
        sh_buf_free(&k);
        return dir;
    }
    char *norm = normalized_url(o->url);
    char *keyed = sh_strfmt("%s#%s", norm, o->ref);
    sh_sha1 h;
    sh_sha1_init(&h);
    sh_sha1_update(&h, keyed, strlen(keyed));
    unsigned char d[20];
    sh_sha1_final(&h, d);
    char hex[41];
    sh_hex(d, 20, hex);
    const char *scheme_end = strstr(norm, "://");
    const char *path = scheme_end ? strchr(scheme_end + 3, '/') : NULL;
    sh_buf p = SH_BUF_INIT;
    for (const char *c = path ? path + 1 : norm; *c; ++c) {
        if (*c == '/') sh_buf_puts(&p, "__");
        else sh_buf_putc(&p, word_char(*c) ? *c : '_');
    }
    char *dir = sh_strfmt("%s/repos/%s/%s-%.8s", cache, *o->host ? o->host : "local", sh_buf_str(&p), hex);
    sh_buf_free(&p);
    free(keyed);
    free(norm);
    return dir;
}
