// See sh_reports.h. What is sent: the repository (owner/repo) and the skill names, to https://www.skills.sh/tele/audit, the endpoint
// `npx skills` uses. The answer is untrusted: it is size-capped, every field is type-checked, and only the three known scanners and
// the risk words upstream defines are kept.
#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE
#endif

#include "sh_reports.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh_fs.h"
#include "sh_http.h"
#include "sh_json.h"
#include "sh_path.h"
#include "sh_proc.h"
#include "sh_state.h"
#include "sh_str.h"
#include "sh_time.h"
#include "sh_utf.h"

#define AUDIT_URL "https://www.skills.sh/tele/audit"
#define DAY_MS (24LL * 60 * 60 * 1000)
#define MAX_ANSWER (256 * 1024)
#define LOOKUP_MS 20000

static const struct {
    const char *key, *label;
} SCANNERS[] = {{"ath", "Gen Agent Trust Hub"}, {"socket", "Socket"}, {"snyk", "Snyk"}};
#define N_SCANNERS (sizeof SCANNERS / sizeof SCANNERS[0])
static const char *const RISKS[] = {"unknown", "safe", "low", "medium", "high", "critical"};

static int rank(const char *risk) {
    for (size_t i = 0; i < sizeof RISKS / sizeof RISKS[0]; ++i)
        if (strcmp(RISKS[i], risk) == 0) return (int)i;
    return 0;
}

const char *sh_report_state_name(sh_report_state s) {
    switch (s) {
    case SH_REPORT_NOT_GITHUB: return "not-github";
    case SH_REPORT_PRIVATE: return "private";
    case SH_REPORT_FAILED: return "failed";
    case SH_REPORT_NONE: return "none";
    case SH_REPORT_OK: return "ok";
    default: return "off";
    }
}

static bool env_says_no(const char *name) {
    char *v = sh_getenv(name);
    const bool no = v && *v && strcmp(v, "0") != 0 && strcmp(v, "false") != 0;
    free(v);
    return no;
}

bool sh_reports_opted_out(void) { return env_says_no("DO_NOT_TRACK") || env_says_no("DISABLE_TELEMETRY"); }
bool sh_reports_enabled(void) { return !sh_reports_opted_out() && sh_setting_bool("security", "reports", false); }

void sh_report_free(sh_report *r) {
    for (size_t i = 0; i < r->n; ++i) {
        free(r->rows[i].risk);
        free(r->rows[i].analyzed_at);
    }
    free(r->source);
    memset(r, 0, sizeof *r);
}

void sh_report_json(yyjson_mut_doc *d, yyjson_mut_val *o, const sh_report *r) {
    yyjson_mut_obj_add_str(d, o, "state", sh_report_state_name(r->state));
    yyjson_mut_obj_add_strcpy(d, o, "source", r->source ? r->source : "");
    yyjson_mut_val *list = yyjson_mut_arr(d);
    for (size_t i = 0; i < r->n; ++i) {
        yyjson_mut_val *x = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, x, "scanner", r->rows[i].scanner);
        yyjson_mut_obj_add_str(d, x, "label", r->rows[i].label);
        yyjson_mut_obj_add_strcpy(d, x, "risk", r->rows[i].risk);
        yyjson_mut_obj_add_int(d, x, "alerts", r->rows[i].alerts);
        yyjson_mut_obj_add_strcpy(d, x, "analyzedAt", r->rows[i].analyzed_at);
        yyjson_mut_arr_append(list, x);
    }
    yyjson_mut_obj_add_val(d, o, "scanners", list);
}

// ---- which repository, and is it public ----

static bool slug_part(const char *s, size_t n) {
    if (n == 0 || n > 100 || (n <= 2 && s[0] == '.')) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
    }
    return true;
}

// "owner/repo" of a GitHub skill, NULL for anything else
static char *github_slug(const sh_source *src) {
    if (!src || !src->source || (src->type && *src->type && strcmp(src->type, "github") != 0)) return NULL;
    const char *s = src->source, *slash = strchr(s, '/');
    if (!slash || strchr(slash + 1, '/') || !slug_part(s, (size_t)(slash - s)) || !slug_part(slash + 1, strlen(slash + 1))) return NULL;
    return sh_strdup(s);
}

static bool test_server(char **url) {  // the tests' server on this machine; anything else is ignored
    char *v = sh_getenv("SKILLHUB_REPORTS_URL");
    if (v && sh_starts_with(v, "http://127.0.0.1:")) {
        *url = v;
        return true;
    }
    free(v);
    *url = sh_strdup(AUDIT_URL);
    return false;
}

// Asked the way a stranger would: no global or system config (no insteadOf rewrite), no credential helper, no prompt.
static bool repo_is_public(const char *slug) {
    char *url_base = NULL, *audit = NULL;
    const bool testing = test_server(&audit);
    free(audit);
    if (testing) url_base = sh_getenv("SKILLHUB_REPORTS_GIT_BASE");
    char *url = url_base && *url_base ? sh_strfmt("%s/%s.git", url_base, slug) : sh_strfmt("https://github.com/%s.git", slug);
    free(url_base);
#ifdef _WIN32
    static const char *const env[] = {"GIT_CONFIG_GLOBAL=NUL", "GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_COUNT=0", "GIT_CONFIG_PARAMETERS=",
                                      "GIT_TERMINAL_PROMPT=0", "GIT_ASKPASS=", "SSH_ASKPASS=", "GCM_INTERACTIVE=never", NULL};
#else
    static const char *const env[] = {"GIT_CONFIG_GLOBAL=/dev/null", "GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_COUNT=0", "GIT_CONFIG_PARAMETERS=",
                                      "GIT_TERMINAL_PROMPT=0", "GIT_ASKPASS=", "SSH_ASKPASS=", "GCM_INTERACTIVE=never", NULL};
#endif
    const char *const argv[] = {sh_git_program(), "-c", "credential.helper=", "-c", "protocol.ext.allow=never", "-c", "protocol.fd.allow=never",
                                "ls-remote", "--heads", "--", url, NULL};
    sh_proc_opts o = {0};
    o.env = env;
    o.timeout_ms = LOOKUP_MS;
    sh_proc_result r = {0};
    const bool ok = sh_proc_run(argv, &o, &r);
    sh_proc_result_free(&r);
    free(url);
    return ok;
}

// ---- the cache: <cache>/reports.json ----

typedef struct {
    yyjson_doc *doc;
    yyjson_val *repos, *skills;
} cache_view;

static char *cache_path(const char *cache) { return sh_path_join(cache, "reports.json"); }

static void cache_open(const char *cache, cache_view *v) {
    memset(v, 0, sizeof *v);
    char *path = cache_path(cache);
    if (sh_json_read(path, 4 * 1024 * 1024, &v->doc) == SH_JSON_OK) {
        yyjson_val *root = yyjson_doc_get_root(v->doc);
        v->repos = sh_json_obj(root, "repos");
        v->skills = sh_json_obj(root, "skills");
    }
    free(path);
}

static bool fresh(yyjson_val *entry) { return entry && sh_now_ms() - sh_json_int(entry, "at", 0) < DAY_MS; }

static char *skill_key(const char *slug, const char *name) { return sh_strfmt("%s|%s", slug, name); }

// keeps what was asked for in `answers`/`public`, on top of what the file holds, and writes it back
static void cache_store(const char *cache, const char *slug, int public_state, const char *const *names, size_t n, yyjson_val *answers) {
    char *path = cache_path(cache);
    yyjson_doc *old = NULL;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = NULL;
    if (sh_json_read(path, 4 * 1024 * 1024, &old) == SH_JSON_OK) root = yyjson_val_mut_copy(d, yyjson_doc_get_root(old));
    if (!root || !yyjson_mut_is_obj(root)) root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    sh_json_set_int(d, root, "version", 1);
    const long long now = sh_now_ms();
    if (public_state >= 0) {
        yyjson_mut_val *repos = sh_json_get_or_add_obj(d, root, "repos"), *r = yyjson_mut_obj(d);
        sh_json_set_bool(d, r, "public", public_state == 1);
        sh_json_set_int(d, r, "at", now);
        sh_json_set_val(d, repos, slug, r);
    }
    if (answers) {
        yyjson_mut_val *skills = sh_json_get_or_add_obj(d, root, "skills");
        for (size_t i = 0; i < n; ++i) {
            yyjson_val *a = yyjson_obj_get(answers, names[i]);
            yyjson_mut_val *e = yyjson_mut_obj(d);
            sh_json_set_int(d, e, "at", now);
            sh_json_set_val(d, e, "scanners", yyjson_is_obj(a) ? yyjson_val_mut_copy(d, a) : yyjson_mut_obj(d));
            char *key = skill_key(slug, names[i]);
            sh_json_set_val(d, skills, key, e);
            free(key);
        }
    }
    sh_mkpath(cache);
    sh_json_write(path, d, false, true);
    yyjson_mut_doc_free(d);
    if (old) yyjson_doc_free(old);
    free(path);
}

// one scanner object as skills.sh writes it: { "risk": "low", "alerts": 0, "score": 90, "analyzedAt": "..." }
static void read_scanners(yyjson_val *scanners, sh_report *out) {
    for (size_t k = 0; k < N_SCANNERS && yyjson_is_obj(scanners); ++k) {
        yyjson_val *x = yyjson_obj_get(scanners, SCANNERS[k].key);
        if (!yyjson_is_obj(x)) continue;
        const char *risk = sh_json_str(x, "risk"), *when = sh_json_str(x, "analyzedAt");
        sh_report_row *row = &out->rows[out->n++];
        row->scanner = SCANNERS[k].key;
        row->label = SCANNERS[k].label;
        row->risk = sh_strdup(rank(risk) || strcmp(risk, "unknown") == 0 ? risk : "unknown");
        yyjson_val *alerts = yyjson_obj_get(x, "alerts");
        row->alerts = yyjson_is_int(alerts) && yyjson_get_int(alerts) >= 0 && yyjson_get_int(alerts) <= 100000 ? (int)yyjson_get_int(alerts) : -1;
        char *clean = sh_printable(strlen(when) <= 40 ? when : "", false);
        row->analyzed_at = clean;
    }
    out->state = out->n ? SH_REPORT_OK : SH_REPORT_NONE;
}

static char *query_encoded(const char *s) {
    sh_buf b = SH_BUF_INIT;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~')
            sh_buf_putc(&b, (char)*p);
        else
            sh_buf_printf(&b, "%%%02X", *p);
    }
    char *out = sh_buf_steal(&b);
    return out ? out : sh_strdup("");
}

// asks skills.sh about `names` of `slug`; the parsed answer, or NULL when it could not be had
static yyjson_doc *ask(const char *cache, const char *slug, const char *const *names, size_t n) {
    char *base = NULL;
    test_server(&base);
    sh_strv list = SH_STRV_INIT;
    for (size_t i = 0; i < n; ++i) sh_strv_push(&list, names[i]);
    char *joined = sh_strv_join(&list, ","), *q_source = query_encoded(slug), *q_skills = query_encoded(joined);
    char *url = sh_strfmt("%s?source=%s&skills=%s", base, q_source, q_skills);
    char *file = sh_strfmt("%s/reports-%ld.json", cache, sh_process_id()), *err = NULL;
    sh_mkpath(cache);
    yyjson_doc *doc = NULL;
    if (sh_http_get(url, file, LOOKUP_MS, &err) && sh_file_size(file) <= MAX_ANSWER) {
        sh_buf b = SH_BUF_INIT;
        if (sh_read_file(file, MAX_ANSWER, &b)) doc = sh_json_parse(sh_buf_str(&b), b.len);
        sh_buf_free(&b);
        if (doc && !yyjson_is_obj(yyjson_doc_get_root(doc))) {
            yyjson_doc_free(doc);
            doc = NULL;
        }
    }
    sh_remove_file(file);
    free(err);
    free(file);
    free(url);
    free(q_skills);
    free(q_source);
    free(joined);
    sh_strv_free(&list);
    free(base);
    return doc;
}

void sh_reports_get(const char *cache, const sh_source *src, const char *const *names, size_t n, bool consent, sh_report *out) {
    for (size_t i = 0; i < n; ++i) memset(&out[i], 0, sizeof out[i]);
    char *slug = github_slug(src);
    for (size_t i = 0; i < n; ++i) out[i].source = sh_strdup(slug ? slug : "");
    sh_report_state state = SH_REPORT_OK;
    if (sh_reports_opted_out() || (!consent && !sh_reports_enabled())) state = SH_REPORT_OFF;
    else if (!slug) state = SH_REPORT_NOT_GITHUB;
    if (state != SH_REPORT_OK) {
        for (size_t i = 0; i < n; ++i) out[i].state = state;
        free(slug);
        return;
    }
    cache_view v;
    cache_open(cache, &v);
    yyjson_val *repo = v.repos ? yyjson_obj_get(v.repos, slug) : NULL;
    int public_state = -1;  // -1: not asked now
    bool is_public;
    if (fresh(repo)) {
        is_public = sh_json_bool(repo, "public");
    } else {
        is_public = repo_is_public(slug);
        public_state = is_public;
    }
    // what the cache answers, and which names still need asking
    sh_strv missing = SH_STRV_INIT;
    for (size_t i = 0; i < n && is_public; ++i) {
        char *key = skill_key(slug, names[i]);
        yyjson_val *e = v.skills ? yyjson_obj_get(v.skills, key) : NULL;
        free(key);
        if (fresh(e)) read_scanners(sh_json_obj(e, "scanners"), &out[i]);
        else sh_strv_push(&missing, names[i]);
    }
    yyjson_doc *answer = NULL;
    if (is_public && missing.len) {
        answer = ask(cache, slug, (const char *const *)missing.items, missing.len);
        for (size_t i = 0; i < n; ++i) {
            if (!sh_strv_contains(&missing, names[i])) continue;
            if (answer) read_scanners(yyjson_obj_get(yyjson_doc_get_root(answer), names[i]), &out[i]);
            else out[i].state = SH_REPORT_FAILED;
        }
    }
    for (size_t i = 0; i < n && !is_public; ++i) out[i].state = SH_REPORT_PRIVATE;
    if (v.doc) yyjson_doc_free(v.doc);
    if (public_state >= 0 || answer)
        cache_store(cache, slug, public_state, (const char *const *)missing.items, answer ? missing.len : 0, answer ? yyjson_doc_get_root(answer) : NULL);
    if (answer) yyjson_doc_free(answer);
    sh_strv_free(&missing);
    free(slug);
}

// ---- the baseline in state.json: { "reports": { "<skill>": { "ath": "safe", "socket": "low", ... } } } ----

static void set_baseline(yyjson_mut_doc *m, const char *name, const sh_report *r) {
    yyjson_mut_val *root = yyjson_mut_doc_get_root(m);
    yyjson_mut_val *all = sh_json_get_or_add_obj(m, root, "reports"), *e = yyjson_mut_obj(m);
    for (size_t i = 0; i < r->n; ++i) sh_json_set_str(m, e, r->rows[i].scanner, r->rows[i].risk);
    sh_json_set_val(m, all, name, e);
}

bool sh_reports_worse_than_baseline(const char *name, const sh_report *now, char **why) {
    if (now->state != SH_REPORT_OK) return false;  // no report, or no answer: nothing to compare, nothing held
    yyjson_mut_doc *m = sh_state_load(NULL);
    if (!m) return false;
    yyjson_mut_val *all = yyjson_mut_obj_get(yyjson_mut_doc_get_root(m), "reports");
    yyjson_mut_val *base = all ? yyjson_mut_obj_get(all, name) : NULL;
    bool worse = false;
    if (!yyjson_mut_is_obj(base)) {
        set_baseline(m, name, now);
        sh_state_save(m);
    } else {
        for (size_t i = 0; i < now->n && !worse; ++i) {
            yyjson_mut_val *was = yyjson_mut_obj_get(base, now->rows[i].scanner);
            const char *before = yyjson_mut_is_str(was) ? yyjson_mut_get_str(was) : "unknown";
            if (rank(now->rows[i].risk) > rank(before)) {
                worse = true;
                *why = sh_strfmt("%s now says %s, it said %s at the last update", now->rows[i].label, now->rows[i].risk, before);
            }
        }
    }
    yyjson_mut_doc_free(m);
    return worse;
}

void sh_reports_remember(const char *cache, const char *name, const sh_source *src) {
    if (!sh_reports_enabled()) return;
    char *slug = github_slug(src);
    if (!slug) return;
    cache_view v;
    cache_open(cache, &v);
    char *key = skill_key(slug, name);
    yyjson_val *e = v.skills ? yyjson_obj_get(v.skills, key) : NULL;
    sh_report r = {0};
    if (e) read_scanners(sh_json_obj(e, "scanners"), &r);
    if (r.state == SH_REPORT_OK) {
        yyjson_mut_doc *m = sh_state_load(NULL);
        if (m) {
            set_baseline(m, name, &r);
            sh_state_save(m);
            yyjson_mut_doc_free(m);
        }
    }
    sh_report_free(&r);
    free(key);
    if (v.doc) yyjson_doc_free(v.doc);
    free(slug);
}
