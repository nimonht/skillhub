#include "sh_git.h"

#include <stdlib.h>
#include <string.h>

#include "sh_fs.h"
#include "sh_path.h"
#include "sh_proc.h"
#include "sh_utf.h"

#ifndef _WIN32
#include <strings.h>
#endif

// ---- running git ----

void sh_git_result_free(sh_git_result *r) {
    sh_buf_free(&r->out);
    free(r->err);
}

// Options for every call: long paths on Windows (some skill repositories nest deeper than 260 characters), no background
// work in the cache clones, which other threads and processes read at the same time, and never the two transports that
// run commands (ext::, fd::), whatever the user's git config says, so a URL in a lock or export file cannot start a program.
static const char *const CONFIG[] = {"core.longpaths=true", "gc.auto=0", "maintenance.auto=false", "core.fsmonitor=false",
                                     "protocol.ext.allow=never", "protocol.fd.allow=never"};
#define N_CONFIG (sizeof CONFIG / sizeof CONFIG[0])

static const char *const *git_argv(const char *const *args) {
    size_t n = 0;
    while (args[n]) ++n;
    const char **argv = sh_xcalloc(1 + 2 * N_CONFIG + n + 1, sizeof *argv);
    size_t k = 0;
    argv[k++] = sh_git_program();
    for (size_t i = 0; i < N_CONFIG; ++i) {
        argv[k++] = "-c";
        argv[k++] = CONFIG[i];
    }
    for (size_t i = 0; i < n; ++i) argv[k++] = args[i];
    return argv;
}

static const char *const GIT_ENV[] = {"GIT_TERMINAL_PROMPT=0", "GCM_INTERACTIVE=never", NULL};

static sh_git_result result_of(sh_proc_result *p) {
    sh_git_result r = {0};
    if (!p->started) {
        r.err = sh_strdup("git was not found: install Git (https://git-scm.com) and make sure it is on your PATH");
    } else if (p->timed_out) {
        r.err = sh_strdup("git timed out");
    } else {
        r.ok = p->exit_code == 0;
        r.err = sh_trimmed(sh_buf_str(&p->err), p->err.len);
    }
    r.out = p->out;
    p->out = (sh_buf)SH_BUF_INIT;
    sh_proc_result_free(p);
    return r;
}

sh_git_result sh_git_run(const char *const *args, const char *work_dir, int timeout_ms) {
    const char *const *argv = git_argv(args);
    const sh_proc_opts o = {work_dir, GIT_ENV, NULL, 0, timeout_ms ? timeout_ms : 180000, NULL, NULL};
    sh_proc_result p;
    sh_proc_run(argv, &o, &p);
    free((void *)argv);
    return result_of(&p);
}

// ---- the cache clone ----

bool sh_repo_cloned(const char *dir) {
    char *dotgit = sh_path_join(dir, ".git");
    const bool yes = sh_exists(dotgit);
    free(dotgit);
    return yes;
}

// the host of a clone URL, lower case; "" for local paths and file:// URLs, which no limit applies to
static char *url_host(const char *url) {
    const char *p = strstr(url, "://");
    const char *start;
    if (p) {
        if (sh_starts_with(url, "file:")) return sh_strdup("");
        start = p + 3;
    } else {
        const char *colon = strchr(url, ':'), *slash = strchr(url, '/');
        if (!colon || (slash && slash < colon) || colon - url < 2) return sh_strdup("");  // a path, or C:\ on Windows
        start = url;
    }
    const char *at = strchr(start, '@'), *end = start + strcspn(start, "/:");
    if (at && at < end) {
        start = at + 1;
        end = start + strcspn(start, "/:");
    }
    char *host = sh_strndup(start, (size_t)(end - start));
    for (char *c = host; *c; ++c)
        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
    return host;
}

// a git command that downloads: at most `fetches` of them per host at a time
static sh_git_result network_git(const char *const *args, const char *work_dir, const char *host, sh_limiter *fetches) {
    if (fetches && *host) sh_limiter_acquire(fetches, host);
    sh_git_result r = sh_git_run(args, work_dir, 0);
    if (fetches && *host) sh_limiter_release(fetches, host);
    return r;
}

static bool failed(sh_git_result *r, char **err) {
    if (r->ok) return false;
    if (err) *err = sh_strdup(r->err ? r->err : "");
    return true;
}

// HEAD's commit and tree from the clone itself, which is the only record of what the cache holds
static bool read_heads(sh_repo *r, char **err) {
    const char *args[] = {"rev-parse", "HEAD", "HEAD^{tree}", NULL};
    sh_git_result g = sh_git_run(args, r->dir, 0);
    bool ok = !failed(&g, err);
    sh_strv lines = SH_STRV_INIT;
    if (ok) sh_split(sh_buf_str(&g.out), g.out.len, '\n', true, &lines);
    if (ok && lines.len != 2) {
        if (err) *err = sh_strdup("unexpected git output for HEAD");
        ok = false;
    }
    if (ok) {
        free(r->head);
        free(r->root);
        r->head = sh_trimmed(lines.items[0], strlen(lines.items[0]));
        r->root = sh_trimmed(lines.items[1], strlen(lines.items[1]));
    }
    sh_strv_free(&lines);
    sh_git_result_free(&g);
    return ok;
}

// how well an `ls-remote` line matches what git fetch would take for `ref`: tags before branches, and for an annotated tag
// the commit it points to ("^{}"), which is what the clone's HEAD holds. ls-remote also lists refs that merely end in
// the name (refs/heads/feature/main for "main"): those do not count.
static int match_rank(const char *name, const char *ref) {
    if (!*ref) return strcmp(name, "HEAD") == 0 ? 1 : 0;
    char *peeled = sh_strfmt("refs/tags/%s^{}", ref), *tag = sh_strfmt("refs/tags/%s", ref), *branch = sh_strfmt("refs/heads/%s", ref);
    const int rank = strcmp(name, peeled) == 0 ? 3 : strcmp(name, tag) == 0 ? 2 : strcmp(name, branch) == 0 ? 1 : 0;
    free(peeled);
    free(tag);
    free(branch);
    return rank;
}

// the commit upstream's `ref` (HEAD when empty) points to, or NULL
static char *remote_head(const sh_repo *r, const char *ref, char **err) {
    char *peeled = *ref ? sh_strfmt("%s^{}", ref) : NULL;
    const char *args[] = {"ls-remote", "--quiet", "--", "origin", *ref ? ref : "HEAD", peeled, NULL};
    sh_git_result g = sh_git_run(args, r->dir, 0);
    free(peeled);
    char *head = NULL;
    if (!failed(&g, err)) {
        sh_strv lines = SH_STRV_INIT;
        sh_split(sh_buf_str(&g.out), g.out.len, '\n', true, &lines);
        int best = 0;
        for (size_t i = 0; i < lines.len; ++i) {
            char *tab = strchr(lines.items[i], '\t');
            if (!tab) continue;
            const int rank = match_rank(tab + 1, ref);
            if (rank > best) {
                best = rank;
                free(head);
                head = sh_strndup(lines.items[i], (size_t)(tab - lines.items[i]));
            }
        }
        sh_strv_free(&lines);
        if (!head && err) *err = *ref ? sh_strfmt("upstream has no branch or tag %s", ref) : sh_strdup("the repository is empty");
    }
    sh_git_result_free(&g);
    return head;
}

static bool update_clone(sh_repo *r, const char *url, const char *ref, sh_limiter *fetches, char **err) {
    char *host = url_host(url);
    bool ok;
    if (!sh_repo_cloned(r->dir)) {
        char *parent = sh_path_parent(r->dir);
        sh_mkpath(parent);
        free(parent);
        sh_remove_tree(r->dir);  // what a broken clone left behind
        // no working tree: everything is read from the object store
        const char *plain[] = {"clone", "--depth", "1", "--no-checkout", "--quiet", "--", url, r->dir, NULL};
        const char *at_ref[] = {"clone", "--depth", "1", "--no-checkout", "--quiet", "--branch", ref, "--", url, r->dir, NULL};
        sh_git_result g = network_git(*ref ? at_ref : plain, NULL, host, fetches);
        ok = !failed(&g, err) && read_heads(r, err);
        sh_git_result_free(&g);
    } else if (!read_heads(r, err)) {
        ok = false;
    } else {
        char *remote = remote_head(r, ref, err);
        ok = remote != NULL;
        if (ok && strcmp(remote, r->head) != 0) {  // upstream moved: download that commit and point the clone at it
            const char *fetch[] = {"fetch", "--depth", "1", "--quiet", "--", "origin", *ref ? ref : "HEAD", NULL};
            sh_git_result g = network_git(fetch, r->dir, host, fetches);
            ok = !failed(&g, err);
            sh_git_result_free(&g);
            if (ok) {
                const char *move[] = {"update-ref", "--no-deref", "HEAD", "FETCH_HEAD^{commit}", NULL};  // a tag peeled to its commit
                g = sh_git_run(move, r->dir, 0);
                ok = !failed(&g, err) && read_heads(r, err);
                sh_git_result_free(&g);
            }
        }
        free(remote);
    }
    free(host);
    return ok;
}

static int cmp_entry(const void *a, const void *b) {
    return strcmp(((const sh_tree_entry *)a)->path, ((const sh_tree_entry *)b)->path);
}

// "<mode> <type> <id>\t<path>\0" records, split in place
static bool read_tree(sh_repo *r, char **err) {
    const char *args[] = {"ls-tree", "-r", "-t", "-z", "--full-tree", "HEAD", NULL};
    sh_git_result g = sh_git_run(args, r->dir, 0);
    if (failed(&g, err)) {
        sh_git_result_free(&g);
        return false;
    }
    r->listing = g.out;
    g.out = (sh_buf)SH_BUF_INIT;
    sh_git_result_free(&g);
    char *p = r->listing.data, *end = p ? p + r->listing.len : NULL;
    size_t cap = 0;
    while (p && p < end) {
        char *rec = p;
        const char *nul = memchr(rec, '\0', (size_t)(end - rec));
        const size_t len = nul ? (size_t)(nul - rec) : (size_t)(end - rec);
        p = rec + len + 1;
        char *tab = memchr(rec, '\t', len);
        if (!tab) continue;
        *tab = '\0';
        char *type = strchr(rec, ' ');
        char *id = type ? strchr(type + 1, ' ') : NULL;
        if (!id) continue;
        *type = '\0';
        *id++ = '\0';
        if (r->n == cap) {
            cap = cap ? cap * 2 : 256;
            r->entries = sh_xrealloc(r->entries, cap * sizeof *r->entries);
        }
        r->entries[r->n++] = (sh_tree_entry){tab + 1, id, (unsigned)strtoul(rec, NULL, 8)};
    }
    if (r->n > 1) qsort(r->entries, r->n, sizeof *r->entries, cmp_entry);
    return true;
}

bool sh_repo_open(sh_repo *r, const char *dir, const char *url, const char *ref, sh_limiter *fetches, char **err) {
    memset(r, 0, sizeof *r);
    r->dir = sh_strdup(dir);
    char *lock_path = sh_strfmt("%s.lock", dir);
    sh_flock *lock = sh_flock_take(lock_path, 10 * 60 * 1000);
    free(lock_path);
    if (!lock) {
        if (err) *err = sh_strfmt("%s is in use by another Skill Hub", dir);
        return false;
    }
    const bool ok = update_clone(r, url, ref, fetches, err) && read_tree(r, err);
    sh_flock_release(lock);
    return ok;
}

void sh_repo_free(sh_repo *r) {
    free(r->dir);
    free(r->head);
    free(r->root);
    free(r->entries);
    sh_buf_free(&r->listing);
    memset(r, 0, sizeof *r);
}

const sh_tree_entry *sh_repo_find(const sh_repo *r, const char *path) {
    if (r->n == 0) return NULL;
    const sh_tree_entry key = {path, NULL, 0};
    return bsearch(&key, r->entries, r->n, sizeof *r->entries, cmp_entry);
}

const char *sh_repo_id(const sh_repo *r, const char *path) {
    if (!*path) return r->root ? r->root : "";
    const sh_tree_entry *e = sh_repo_find(r, path);
    return e ? e->id : "";
}

// ---- reading many objects ----

typedef struct {
    const sh_blob_reader *rd;
    size_t n, i;     // objects asked for, the one being read
    int state;       // 0: header line, 1: content, 2: the newline after the content
    bool skip;       // content of an object the reader was told is missing (not a file)
    char head[256];
    size_t head_len;
    long long left;  // content bytes still to come
    bool broken;
} batch;

// "<id> blob <size>" -> size; "<id> missing", other types or anything unexpected -> -1. *has_content: content bytes
// follow the line (other types have them too)
static long long header_size(char *line, bool *has_content, long long *content) {
    *has_content = false;
    sh_strv f = SH_STRV_INIT;
    sh_split(line, strlen(line), ' ', true, &f);
    long long size = -1;
    if (f.len == 3) {
        char *endp = NULL;
        const long long n = strtoll(f.items[2], &endp, 10);
        if (endp && !*endp && n >= 0) {
            *has_content = true;
            *content = n;
            if (strcmp(f.items[1], "blob") == 0) size = n;
        }
    }
    sh_strv_free(&f);
    return size;
}

static void next_object(batch *b) {
    b->rd->end(b->rd->user, b->i);
    ++b->i;
    b->state = 0;
}

static bool on_batch_header(batch *b, const char **p, size_t *n) {
    const char *nl = memchr(*p, '\n', *n);
    const size_t take = nl ? (size_t)(nl - *p) : *n;
    if (b->head_len + take >= sizeof b->head) return false;
    memcpy(b->head + b->head_len, *p, take);
    b->head_len += take;
    *p += take;
    *n -= take;
    if (!nl) return true;
    ++*p;
    --*n;
    b->head[b->head_len] = '\0';
    b->head_len = 0;
    if (b->i >= b->n) return false;
    bool has_content;
    long long content = 0;
    const long long size = header_size(b->head, &has_content, &content);
    b->rd->begin(b->rd->user, b->i, size);
    b->skip = size < 0;
    if (!has_content) {
        next_object(b);
    } else {
        b->left = content;
        b->state = content > 0 ? 1 : 2;
    }
    return true;
}

static bool on_batch(void *user, const char *p, size_t n) {
    batch *b = user;
    while (n > 0 && !b->broken) {
        if (b->state == 0) {
            if (!on_batch_header(b, &p, &n)) b->broken = true;
        } else if (b->state == 1) {
            const size_t take = (long long)n < b->left ? n : (size_t)b->left;
            if (!b->skip) b->rd->data(b->rd->user, b->i, p, take);
            p += take;
            n -= take;
            b->left -= (long long)take;
            if (b->left == 0) b->state = 2;
        } else if (*p != '\n') {
            b->broken = true;
        } else {
            ++p;
            --n;
            next_object(b);
        }
    }
    return !b->broken;
}

bool sh_repo_read(const sh_repo *r, const char *const *ids, size_t n, const sh_blob_reader *reader, char **err) {
    if (n == 0) return true;
    sh_buf input = SH_BUF_INIT;
    for (size_t i = 0; i < n; ++i) sh_buf_printf(&input, "%s\n", ids[i]);
    batch b;
    memset(&b, 0, sizeof b);
    b.rd = reader;
    b.n = n;
    const char *args[] = {"cat-file", "--batch", NULL};
    const char *const *argv = git_argv(args);
    const int timeout = 120000 + (int)(n < 100000 ? n : 100000) * 20;
    const sh_proc_opts o = {r->dir, GIT_ENV, input.data, input.len, timeout, on_batch, &b};
    sh_proc_result p;
    const bool ran = sh_proc_run(argv, &o, &p);
    free((void *)argv);
    sh_buf_free(&input);
    const bool ok = ran && !b.broken && b.i == n;
    if (!ok && err) *err = sh_strdup(!p.started ? "cannot start git" : "git cat-file failed");
    sh_proc_result_free(&p);
    return ok;
}

// ---- extracting folders ----

// a path below a skill folder that may be written: relative, no "." or ".." parts, no backslash, no .git folder anywhere
static bool path_is_safe(const char *path) {
    if (!*path || path[0] == '/' || strchr(path, '\\')) return false;
    sh_strv seg = SH_STRV_INIT;
    sh_split(path, strlen(path), '/', false, &seg);
    bool sane = true;
    for (size_t i = 0; sane && i < seg.len; ++i) {
        const char *part = seg.items[i];
        if (!*part || strcmp(part, "..") == 0 || strcmp(part, ".") == 0) sane = false;
#ifdef _WIN32
        else if (_stricmp(part, ".git") == 0 || sh_path_windows_unsafe(part)) sane = false;  // nor a name Windows tools could not remove
#else
        else if (strcasecmp(part, ".git") == 0) sane = false;
#endif
    }
    sh_strv_free(&seg);
    return sane;
}

typedef struct {
    const char *rel;  // below the folder
    const char *id;
    bool exec;
    size_t job;
} file_item;

typedef struct {
    sh_extract *jobs;
    file_item *files;
    long long *bytes;  // per job
    sh_file *out;      // the file being written
    char *out_path;
} extraction;

static void fail_job(sh_extract *j, char *why) {
    if (j->ok) {
        j->ok = false;
        j->err = why;
    } else {
        free(why);
    }
}

static void x_begin(void *user, size_t i, long long size) {
    extraction *x = user;
    const file_item *f = &x->files[i];
    sh_extract *j = &x->jobs[f->job];
    if (!j->ok) return;
    if (size < 0) {
        fail_job(j, sh_strfmt("unexpected git output for %s", f->rel));
        return;
    }
    x->bytes[f->job] += size;
    if (x->bytes[f->job] > SH_SKILL_MAX_BYTES) {
        fail_job(j, sh_strdup("more than 128 MB"));
        return;
    }
    x->out_path = sh_path_join(j->dest, f->rel);
    char *parent = sh_path_parent(x->out_path);
    sh_mkpath(parent);
    free(parent);
    x->out = sh_file_create(x->out_path);
    if (!x->out) fail_job(j, sh_strfmt("cannot write %s", f->rel));
}

static void x_data(void *user, size_t i, const char *p, size_t n) {
    extraction *x = user;
    (void)i;
    if (x->out) sh_file_write(x->out, p, n);
}

static void x_end(void *user, size_t i) {
    extraction *x = user;
    const file_item *f = &x->files[i];
    if (x->out) {
        if (!sh_file_close(x->out)) fail_job(&x->jobs[f->job], sh_strfmt("cannot write %s", f->rel));
        else sh_set_executable(x->out_path, f->exec);
        x->out = NULL;
    }
    free(x->out_path);
    x->out_path = NULL;
}

// the first entry at or after `path` in byte order
static size_t lower_bound(const sh_repo *r, const char *path) {
    size_t lo = 0, hi = r->n;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (strcmp(r->entries[mid].path, path) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// the plain files of job j's folder, appended to `files`
static void list_files(const sh_repo *r, sh_extract *j, size_t job, file_item **files, size_t *n, size_t *cap) {
    char *prefix = *j->dir ? sh_strfmt("%s/", j->dir) : sh_strdup("");
    const size_t plen = strlen(prefix);
    size_t count = 0;
    const size_t first = *n;
    for (size_t e = lower_bound(r, prefix); e < r->n && sh_starts_with(r->entries[e].path, prefix); ++e) {
        const sh_tree_entry *t = &r->entries[e];
        // plain files only: links (120000) are never recreated, submodules and folders are not files
        if ((t->mode != SH_MODE_FILE && t->mode != SH_MODE_EXEC) || !path_is_safe(t->path + plen)) continue;
        if (*n == *cap) {
            *cap = *cap ? *cap * 2 : 64;
            *files = sh_xrealloc(*files, *cap * sizeof **files);
        }
        (*files)[(*n)++] = (file_item){t->path + plen, t->id, t->mode == SH_MODE_EXEC, job};
        ++count;
    }
    free(prefix);
    if (count > SH_SKILL_MAX_FILES) {
        fail_job(j, sh_strdup("more than 5000 files"));
        *n = first;
    }
}

void sh_repo_extract(const sh_repo *r, sh_extract *jobs, size_t n) {
    file_item *files = NULL;
    size_t n_files = 0, cap = 0;
    for (size_t j = 0; j < n; ++j) {
        jobs[j].ok = true;
        jobs[j].err = NULL;
        list_files(r, &jobs[j], j, &files, &n_files, &cap);
        if (jobs[j].ok) sh_mkpath(jobs[j].dest);
    }
    const char **ids = sh_xcalloc(n_files + 1, sizeof *ids);
    for (size_t i = 0; i < n_files; ++i) ids[i] = files[i].id;
    extraction x = {jobs, files, sh_xcalloc(n + 1, sizeof(long long)), NULL, NULL};
    const sh_blob_reader rd = {x_begin, x_data, x_end, &x};
    char *why = NULL;
    if (!sh_repo_read(r, ids, n_files, &rd, &why)) {
        if (x.out) {
            sh_file_close(x.out);
            free(x.out_path);
        }
        for (size_t i = 0; i < n_files; ++i)
            if (jobs[files[i].job].ok) fail_job(&jobs[files[i].job], sh_strdup(why));
    }
    free(why);
    free(x.bytes);
    free(ids);
    free(files);
}
