// `skillhub serve --stdio`: the backend the window talks to (docs/protocol.md). Three threads besides the loop: one reads
// standard input, one writes standard output, and each job and each scan run on their own. Only the loop thread touches
// the server's state; everyone else posts events to it.
#include "sh_serve.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "serve.h"
#include "sh_fs.h"
#include "sh_json.h"
#include "sh_link.h"
#include "sh_path.h"
#include "sh_proc.h"
#include "sh_utf.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define PROTOCOL 1
#define BACKPRESSURE (8u * 1024 * 1024)  // queued output above this: deltas give way to one state.resync
#define PREVIEW_MAX (80 * 1024)

static server sv;  // one per process; static, so a leak checker sees what it still holds at exit

// ---- events ----

void post_event(server *s, event_kind kind, job *j, char *text, size_t len, long a, long b, void *data) {
    event *ev = sh_xcalloc(1, sizeof *ev);
    *ev = (event){kind, text, len, j, a, b, data, NULL};
    sh_mutex_lock(s->m);
    if (s->closed) {  // the loop is gone: nobody will read it
        sh_mutex_unlock(s->m);
        free(ev->text);
        free(ev);
        return;
    }
    if (s->tail) s->tail->next = ev;
    else s->head = ev;
    s->tail = ev;
    uv_async_send(&s->wake);
    sh_mutex_unlock(s->m);
}

// ---- output ----

struct out_line {
    char *s;
    size_t n;
    struct out_line *next;
};

void send_line(server *s, char *line) {
    if (!line) return;
    struct out_line *o = sh_xcalloc(1, sizeof *o);
    o->s = line;
    o->n = strlen(line);
    sh_mutex_lock(s->out_m);
    if (s->out_failed) {
        sh_mutex_unlock(s->out_m);
        free(line);
        free(o);
        return;
    }
    if (s->out_tail) s->out_tail->next = o;
    else s->out_head = o;
    s->out_tail = o;
    s->out_bytes += o->n;
    sh_cond_broadcast(s->out_cv);
    sh_mutex_unlock(s->out_m);
}

size_t output_queued(server *s) {
    sh_mutex_lock(s->out_m);
    const size_t n = s->out_bytes;
    sh_mutex_unlock(s->out_m);
    return n;
}

void send_result(server *s, sh_rpc_msg *m, yyjson_mut_doc *d, yyjson_mut_val *result) { send_line(s, sh_rpc_result(d, m->id, result)); }

void send_error(server *s, sh_rpc_msg *m, int code, const char *message) { send_line(s, sh_rpc_error(m ? m->id : NULL, code, message)); }

void send_notification(server *s, yyjson_mut_doc *d, const char *method, yyjson_mut_val *params) {
    send_line(s, sh_rpc_notification(d, method, params));
}

static bool write_all(int fd, const char *p, size_t n) {
    while (n > 0) {
#ifdef _WIN32
        const int w = _write(fd, p, n > 1u << 30 ? 1u << 30 : (unsigned)n);
#else
        const ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
#endif
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static void writer_main(void *arg) {
    server *s = arg;
    for (;;) {
        sh_mutex_lock(s->out_m);
        while (!s->out_head && !s->out_closing) sh_cond_wait(s->out_cv, s->out_m);
        struct out_line *o = s->out_head;
        if (!o) {  // closing, and everything is out
            sh_mutex_unlock(s->out_m);
            return;
        }
        s->out_head = o->next;
        if (!s->out_head) s->out_tail = NULL;
        sh_mutex_unlock(s->out_m);
        const bool ok = write_all(s->out_fd, o->s, o->n);
        sh_mutex_lock(s->out_m);
        s->out_bytes -= o->n;
        if (!ok) s->out_failed = true;
        sh_mutex_unlock(s->out_m);
        free(o->s);
        free(o);
        if (!ok) {
            post_event(s, EV_WRITER_GONE, NULL, NULL, 0, 0, 0, NULL);
            return;
        }
    }
}

// ---- input ----

static void on_input_line(void *user, const char *line, size_t len) {
    char *copy = NULL;
    if (line) {  // a NULL line was too long: the loop answers with an error
        copy = sh_xmalloc(len + 1);
        memcpy(copy, line, len);
        copy[len] = '\0';
    }
    post_event(user, EV_LINE, NULL, copy, len, line != NULL, 0, NULL);
}

static void reader_main(void *arg) {
    server *s = arg;
    sh_rpc_framer *f = sh_rpc_framer_new(SH_RPC_MAX_LINE);
    char buf[65536];
    for (;;) {
#ifdef _WIN32
        const int n = _read(0, buf, sizeof buf);
#else
        const ssize_t n = read(0, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
#endif
        if (n <= 0) break;
        sh_rpc_feed(f, buf, (size_t)n, on_input_line, s);
    }
    sh_rpc_framer_free(f);
    post_event(s, EV_EOF, NULL, NULL, 0, 0, 0, NULL);
}

// ---- parameters ----

const char *param_str(yyjson_val *params, const char *key) {
    const char *v = yyjson_get_str(yyjson_obj_get(params, key));
    return v ? v : "";
}

bool param_bool(yyjson_val *params, const char *key, bool fallback) {
    yyjson_val *v = yyjson_obj_get(params, key);
    return yyjson_is_bool(v) ? yyjson_get_bool(v) : fallback;
}

void param_strv(yyjson_val *params, const char *key, sh_strv *out) {
    yyjson_val *arr = yyjson_obj_get(params, key);
    if (!yyjson_is_arr(arr)) return;
    size_t i, n;
    yyjson_val *v;
    yyjson_arr_foreach(arr, i, n, v)
        if (yyjson_is_str(v)) sh_strv_push(out, yyjson_get_str(v));
}

// ---- merged reporting ----

static void flush_when_reading(server *s);

static void on_flush(uv_timer_t *t) { flush_when_reading(t->data); }

static void arm_flush(server *s) {
    if (!uv_is_active((uv_handle_t *)&s->flush)) uv_timer_start(&s->flush, on_flush, 50, 0);
}

static void send_progress_and_logs(server *s) {
    if (s->progress_dirty && s->job) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, p, "job", s->job->name);
        yyjson_mut_obj_add_int(d, p, "done", s->progress_done);
        yyjson_mut_obj_add_int(d, p, "total", s->progress_total);
        yyjson_mut_obj_add_strcpy(d, p, "current", s->progress_current ? s->progress_current : "");
        send_notification(s, d, "event.progress", p);
        yyjson_mut_doc_free(d);
    }
    s->progress_dirty = false;
    if (s->pending_logs.len) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = yyjson_mut_obj(d), *lines = yyjson_mut_arr(d);
        for (size_t i = 0; i < s->pending_logs.len; ++i) yyjson_mut_arr_append(lines, yyjson_mut_rawcpy(d, s->pending_logs.items[i]));
        yyjson_mut_obj_add_val(d, p, "lines", lines);
        send_notification(s, d, "event.log", p);
        yyjson_mut_doc_free(d);
        sh_strv_clear(&s->pending_logs);
    }
}

// on the timer: when the window is not reading, progress and log lines keep merging instead of piling up
static void flush_when_reading(server *s) {
    if (output_queued(s) > BACKPRESSURE) {
        arm_flush(s);
        return;
    }
    send_progress_and_logs(s);
}

void flush_pending(server *s) { send_progress_and_logs(s); }

void loop_log(void *user, const char *line) {
    server *s = user;
    sh_strv_take(&s->pending_logs, log_line_json(line));
    arm_flush(s);
}

// ---- scans ----

struct waiting {
    sh_rpc_msg *msg;
    waiting *next;
};

typedef struct {
    server *sv;
    check_map checks;
    sh_strv just_updated;
} scan_args;

static void scan_main(void *arg) {
    scan_args *a = arg;
    scan_result *r = scan_build(&a->checks, &a->just_updated);
    check_map_free(&a->checks);
    sh_strv_free(&a->just_updated);
    post_event(a->sv, EV_SCAN_DONE, NULL, NULL, 0, 0, 0, r);
    free(a);
}

void request_scan(server *s) {
    if (s->scan_running) {
        s->scan_again = true;
        return;
    }
    scan_args *a = sh_xcalloc(1, sizeof *a);
    a->sv = s;
    a->checks = check_map_copy(&s->checks);
    for (size_t i = 0; i < s->just_updated.len; ++i) sh_strv_push(&a->just_updated, s->just_updated.items[i]);
    s->scanner = sh_thread_start(scan_main, a);
    s->scan_running = s->scanner != NULL;
    if (!s->scanner) {
        check_map_free(&a->checks);
        sh_strv_free(&a->just_updated);
        free(a);
    }
}

void wait_for_scan(server *s, sh_rpc_msg *m) {
    waiting *w = sh_xcalloc(1, sizeof *w);
    w->msg = m;
    w->next = s->waiting;
    s->waiting = w;
}

static void answer(server *s, sh_rpc_msg *m);

static void scan_done(server *s, scan_result *r) {
    sh_thread_join(s->scanner);
    s->scanner = NULL;
    s->scan_running = false;
    for (size_t i = 0; i < r->stale.len; ++i) check_map_remove(&s->checks, r->stale.items[i]);
    if (s->state) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *delta = scan_delta(d, s->state, r, s->rev, s->rev + 1);
        if (delta) {
            ++s->rev;
            if (output_queued(s) > BACKPRESSURE) s->resync_pending = true;  // the window takes a fresh snapshot instead
            else send_notification(s, d, "state.delta", delta);
        }
        yyjson_mut_doc_free(d);
    } else {
        s->rev = 1;
    }
    scan_result_free(s->state);
    s->state = r;
    if (s->resync_pending && output_queued(s) < BACKPRESSURE / 8) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        send_notification(s, d, "state.resync", yyjson_mut_obj(d));
        yyjson_mut_doc_free(d);
        s->resync_pending = false;
    }
    watch_sync(s, &r->watch_dirs, &r->watch_files);
    waiting *w = s->waiting;  // answered in the order they came
    s->waiting = NULL;
    waiting *rev = NULL;
    while (w) {
        waiting *next = w->next;
        w->next = rev;
        rev = w;
        w = next;
    }
    while (rev) {
        waiting *next = rev->next;
        answer(s, rev->msg);
        sh_rpc_msg_free(rev->msg);
        free(rev->msg);
        free(rev);
        rev = next;
    }
    if (s->scan_again) {
        s->scan_again = false;
        request_scan(s);
    }
}

// ---- the mutation lock ----

bool mutation_lock_try(sh_flock **held) {
    *held = NULL;
    char *state = sh_state_dir();
    char *path = sh_path_join(state, "mutate.lock");
    if (*state) *held = sh_flock_take(path, 0);
    const bool ok = !*state || *held;
    free(path);
    free(state);
    return ok;
}

// immediate methods that write config the jobs also read
static bool may_write(server *s, sh_rpc_msg *m, sh_flock **lock) {
    if (s->job) {
        send_error(s, m, SH_RPC_BUSY, "another job is running");
        return false;
    }
    if (!mutation_lock_try(lock)) {
        send_error(s, m, SH_RPC_LOCKED, "another Skill Hub is changing skills (a command line run, or the daily update)");
        return false;
    }
    return true;
}

// ---- settings ----

static char *settings_path(void) {
    char *config = sh_config_dir();
    char *p = sh_path_join(config, "settings.json");
    free(config);
    return p;
}

// settings.json as a mutable object; NULL when the file exists but cannot be parsed (it is then never overwritten)
static yyjson_mut_doc *settings_load(void) {
    char *path = settings_path();
    yyjson_doc *doc = NULL;
    const sh_json_status st = sh_json_read(path, 4 * 1024 * 1024, &doc);
    free(path);
    if (st != SH_JSON_OK && st != SH_JSON_MISSING) return NULL;
    yyjson_mut_doc *m = doc ? yyjson_doc_mut_copy(doc, NULL) : yyjson_mut_doc_new(NULL);
    if (!doc) yyjson_mut_doc_set_root(m, yyjson_mut_obj(m));
    yyjson_doc_free(doc);
    return m;
}

void settings_announce(server *s) {
    yyjson_mut_doc *m = settings_load();
    if (!m) return;
    char *json = yyjson_mut_write(m, 0, NULL);
    if (json && (!s->settings_seen || strcmp(json, s->settings_seen) != 0)) {
        free(s->settings_seen);
        s->settings_seen = json;
        yyjson_mut_val *p = yyjson_mut_obj(m);
        yyjson_mut_obj_add_val(m, p, "settings", yyjson_mut_doc_get_root(m));
        send_notification(s, m, "settings.changed", p);
    } else {
        free(json);
    }
    yyjson_mut_doc_free(m);
}

// ---- immediate methods ----

static void m_initialize(server *s, sh_rpc_msg *m) {
    yyjson_val *proto = yyjson_obj_get(m->params, "protocol");
    if (!yyjson_is_int(proto) || yyjson_get_int(proto) != PROTOCOL) {
        send_error(s, m, SH_RPC_PROTOCOL_MISMATCH, "backend and window are from different versions");
        return;
    }
    s->initialized = true;
    sh_store st;
    sh_store_init(&st, NULL, NULL);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *r = yyjson_mut_obj(d), *caps = yyjson_mut_obj(d), *paths = yyjson_mut_obj(d);
    yyjson_mut_obj_add_int(d, r, "protocol", PROTOCOL);
    yyjson_mut_obj_add_str(d, r, "version", SKILLHUB_VERSION);
    yyjson_mut_obj_add_int(d, r, "pid", sh_process_id());
    yyjson_mut_obj_add_str(d, caps, "automation", sh_automation_supported() ? "systemd" : "none");
    yyjson_mut_obj_add_str(d, caps, "links", sh_can_make_dir_links() ? "symlink" : "junction");
    yyjson_mut_obj_add_bool(d, caps, "projects", false);
    yyjson_mut_obj_add_bool(d, caps, "terminalFetch", false);
    yyjson_mut_obj_add_val(d, r, "capabilities", caps);
    char *config = sh_config_dir();
    yyjson_mut_obj_add_strcpy(d, paths, "home", st.home);
    yyjson_mut_obj_add_strcpy(d, paths, "central", st.central);
    yyjson_mut_obj_add_strcpy(d, paths, "backup", st.backup);
    yyjson_mut_obj_add_strcpy(d, paths, "cache", st.cache);
    yyjson_mut_obj_add_strcpy(d, paths, "state", st.state);
    yyjson_mut_obj_add_strcpy(d, paths, "config", config);
    yyjson_mut_obj_add_strcpy(d, paths, "lock", st.lock_path);
    yyjson_mut_obj_add_val(d, r, "paths", paths);
    send_result(s, m, d, r);
    size_t only_other = 0;
    if (sh_lock_split(&st, &only_other)) {  // the window offers the one-time merge
        yyjson_mut_val *p = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, p, "primary", st.lock_path);
        yyjson_mut_obj_add_strcpy(d, p, "other", st.lock_other);
        yyjson_mut_obj_add_uint(d, p, "onlyInOther", only_other);
        send_notification(s, d, "lock.split", p);
    }
    free(config);
    yyjson_mut_doc_free(d);
    sh_store_free(&st);
}

static void reply_shutdown(server *s, const char *cancelled_job) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *r = yyjson_mut_obj(d), *list = yyjson_mut_arr(d);
    if (cancelled_job) yyjson_mut_arr_add_strcpy(d, list, cancelled_job);
    yyjson_mut_obj_add_val(d, r, "cancelled", list);
    send_result(s, s->shutdown_msg, d, r);
    yyjson_mut_doc_free(d);
    sh_rpc_msg_free(s->shutdown_msg);
    free(s->shutdown_msg);
    s->shutdown_msg = NULL;
}

static bool m_shutdown(server *s, sh_rpc_msg *m) {
    s->shutting_down = true;
    s->shutdown_msg = m;
    if (!s->job) {
        reply_shutdown(s, NULL);
    } else {
        job_cancel(s->job);  // answered when it has stopped at a safe point
    }
    return true;
}

static void m_snapshot_page(server *s, sh_rpc_msg *m) {
    yyjson_val *page = yyjson_obj_get(m->params, "page"), *size = yyjson_obj_get(m->params, "pageSize");
    const size_t pg = yyjson_is_uint(page) ? (size_t)yyjson_get_uint(page) : 0;
    const size_t ps = yyjson_is_uint(size) && yyjson_get_uint(size) > 0 ? (size_t)yyjson_get_uint(size) : 1000;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    send_result(s, m, d, scan_page(d, s->state, s->rev, pg, ps));
    yyjson_mut_doc_free(d);
}

static void m_rescan_result(server *s, sh_rpc_msg *m) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *r = yyjson_mut_obj(d);
    yyjson_mut_obj_add_int(d, r, "managed", s->state->managed);
    yyjson_mut_obj_add_int(d, r, "known", s->state->known);
    send_result(s, m, d, r);
    yyjson_mut_doc_free(d);
}

static void m_providers_list(server *s, sh_rpc_msg *m) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *r = yyjson_mut_obj(d);
    yyjson_mut_obj_add_val(d, r, "providers", yyjson_mut_rawcpy(d, s->state->providers));
    send_result(s, m, d, r);
    yyjson_mut_doc_free(d);
}

// a request answered from the state: right away, or after the scan that brings it up to date
static void answer(server *s, sh_rpc_msg *m) {
    if (strcmp(m->method, "providers.rescan") == 0) m_rescan_result(s, m);
    else if (strcmp(m->method, "providers.list") == 0) m_providers_list(s, m);
    else m_snapshot_page(s, m);
}

static bool m_from_state(server *s, sh_rpc_msg *m) {
    const bool fresh = strcmp(m->method, "providers.rescan") == 0 || param_bool(m->params, "rescan", false);
    if (!fresh && s->state) {
        answer(s, m);
        return false;
    }
    wait_for_scan(s, m);
    if (fresh) request_scan(s);  // else the first scan is still running
    return true;
}

static void m_read(server *s, sh_rpc_msg *m) {
    const char *name = param_str(m->params, "name");
    if (!*name || strchr(name, '/') || strchr(name, '\\') || name[0] == '.') {
        send_error(s, m, SH_RPC_INVALID_PARAMS, "not a skill name");
        return;
    }
    sh_store st;
    sh_store_init(&st, NULL, NULL);
    char *path = sh_strfmt("%s/%s/SKILL.md", st.central, name);
    sh_buf b = SH_BUF_INIT;
    if (!sh_read_file(path, PREVIEW_MAX, &b)) {
        send_error(s, m, SH_RPC_NOT_FOUND, "no such skill");
    } else {
        char *text = sh_utf8_clean(b.data ? b.data : "", b.len);  // a cut in the middle of a character is replaced
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *r = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, r, "name", name);
        yyjson_mut_obj_add_strcpy(d, r, "text", text);
        yyjson_mut_obj_add_bool(d, r, "truncated", b.len >= PREVIEW_MAX);
        send_result(s, m, d, r);
        yyjson_mut_doc_free(d);
        free(text);
    }
    sh_buf_free(&b);
    free(path);
    sh_store_free(&st);
}

static void m_providers_add(server *s, sh_rpc_msg *m) {
    sh_flock *lock = NULL;
    if (!may_write(s, m, &lock)) return;
    sh_store st;
    sh_store_init(&st, loop_log, s);
    char *err = NULL;
    const bool shared = param_bool(m->params, "readsShared", false);
    char *id = sh_store_add_custom_provider(&st, param_str(m->params, "label"), param_str(m->params, "dir"), shared ? "native" : "link", &err);
    if (!id) {
        send_error(s, m, SH_RPC_REFUSED, err ? err : "refused");
    } else {
        sh_store_sync_watcher(&st);
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *r = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, r, "id", id);
        send_result(s, m, d, r);
        yyjson_mut_doc_free(d);
        request_scan(s);
    }
    free(id);
    free(err);
    sh_store_free(&st);
    sh_flock_release(lock);
}

static void m_providers_remove(server *s, sh_rpc_msg *m) {
    sh_flock *lock = NULL;
    if (!may_write(s, m, &lock)) return;
    sh_store st;
    sh_store_init(&st, loop_log, s);
    if (!sh_store_remove_custom_provider(&st, param_str(m->params, "id"))) {
        send_error(s, m, SH_RPC_NOT_FOUND, "no agent of yours has that id");
    } else {
        sh_store_sync_watcher(&st);
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        send_result(s, m, d, yyjson_mut_obj(d));
        yyjson_mut_doc_free(d);
        request_scan(s);
    }
    sh_store_free(&st);
    sh_flock_release(lock);
}

static void m_export(server *s, sh_rpc_msg *m) {
    sh_store st;
    sh_store_init(&st, loop_log, s);
    int tracked = 0, local = 0;
    sh_strv skipped = SH_STRV_INIT;
    char *err = NULL;
    if (!sh_store_write_export(&st, param_str(m->params, "path"), param_bool(m->params, "includeLocal", true), &tracked, &local, &skipped, &err)) {
        send_error(s, m, SH_RPC_REFUSED, err ? err : "could not write the file");
    } else {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *r = yyjson_mut_obj(d), *list = yyjson_mut_arr(d);
        for (size_t i = 0; i < skipped.len; ++i) yyjson_mut_arr_add_strcpy(d, list, skipped.items[i]);
        yyjson_mut_obj_add_int(d, r, "tracked", tracked);
        yyjson_mut_obj_add_int(d, r, "local", local);
        yyjson_mut_obj_add_val(d, r, "skipped", list);
        send_result(s, m, d, r);
        yyjson_mut_doc_free(d);
    }
    free(err);
    sh_strv_free(&skipped);
    sh_store_free(&st);
}

static void m_preview(server *s, sh_rpc_msg *m) {
    sh_store st;
    sh_store_init(&st, NULL, NULL);
    sh_import_items items;
    char *err = NULL;
    sh_store_read_export(&st, param_str(m->params, "path"), &items, &err);
    if (items.len == 0) {
        send_error(s, m, SH_RPC_REFUSED, err ? err : "nothing to import in that file");
    } else {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *r = yyjson_mut_obj(d), *list = yyjson_mut_arr(d);
        for (size_t i = 0; i < items.len; ++i) {
            const sh_import_item *it = &items.items[i];
            yyjson_mut_val *o = yyjson_mut_obj(d);
            yyjson_mut_obj_add_strcpy(d, o, "name", it->name);
            yyjson_mut_obj_add_strcpy(d, o, "description", it->description ? it->description : "");
            yyjson_mut_obj_add_bool(d, o, "local", it->local);
            yyjson_mut_obj_add_strcpy(d, o, "source", it->source ? it->source : "");
            yyjson_mut_obj_add_uint(d, o, "files", it->n_files);
            yyjson_mut_obj_add_bool(d, o, "exists", it->exists);
            yyjson_mut_obj_add_strcpy(d, o, "problem", it->problem ? it->problem : "");
            yyjson_mut_obj_add_bool(d, o, "installable", !(it->problem && *it->problem) && !it->exists);
            yyjson_mut_arr_append(list, o);
        }
        yyjson_mut_obj_add_val(d, r, "items", list);
        send_result(s, m, d, r);
        yyjson_mut_doc_free(d);
    }
    free(err);
    sh_import_items_free(&items);
    sh_store_free(&st);
}

static void automation_result(server *s, sh_rpc_msg *m, sh_store *st) {
    char *timer = sh_store_unit_state(st, "skillhub-update.timer"), *watcher = sh_store_unit_state(st, "skillhub-adopt.path");
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *r = yyjson_mut_obj(d);
    const bool supported = sh_automation_supported();
    yyjson_mut_obj_add_bool(d, r, "supported", supported);
    yyjson_mut_obj_add_str(d, r, "timer", strcmp(timer, "enabled") == 0 ? "on" : "off");
    yyjson_mut_obj_add_str(d, r, "watcher", strcmp(watcher, "enabled") == 0 ? "on" : "off");
    yyjson_mut_obj_add_str(d, r, "mechanism", supported ? "systemd" : "none");
    send_result(s, m, d, r);
    yyjson_mut_doc_free(d);
    free(timer);
    free(watcher);
}

static void m_automation_status(server *s, sh_rpc_msg *m) {
    sh_store st;
    sh_store_init(&st, NULL, NULL);
    automation_result(s, m, &st);
    sh_store_free(&st);
}

static void m_automation_set(server *s, sh_rpc_msg *m) {
    sh_flock *lock = NULL;
    if (!may_write(s, m, &lock)) return;
    sh_store st;
    sh_store_init(&st, loop_log, s);
    char *err = NULL;
    bool ok = true;
    static const char *const keys[] = {"timer", "watcher"};
    for (size_t i = 0; i < 2 && ok; ++i) {
        yyjson_val *v = yyjson_obj_get(m->params, keys[i]);
        if (!yyjson_is_bool(v)) continue;
        const bool timer = i == 0;
        ok = yyjson_get_bool(v) ? sh_store_install_units(&st, timer, !timer, &err) : sh_store_remove_units(&st, timer, !timer, &err);
    }
    if (ok) automation_result(s, m, &st);
    else send_error(s, m, SH_RPC_REFUSED, err ? err : "could not change the automation");
    free(err);
    sh_store_free(&st);
    sh_flock_release(lock);
}

static void m_settings_get(server *s, sh_rpc_msg *m) {
    yyjson_mut_doc *d = settings_load();
    if (!d) {
        send_error(s, m, SH_RPC_UNPARSABLE, "settings.json cannot be read; it is left as it is until it is fixed");
        return;
    }
    send_result(s, m, d, yyjson_mut_doc_get_root(d));
    yyjson_mut_doc_free(d);
}

// Settings are not skills: jobs never read or write them, so changing one needs no mutation lock and works mid-job.
static void m_settings_set(server *s, sh_rpc_msg *m) {
    yyjson_val *patch = yyjson_obj_get(m->params, "patch");
    if (!yyjson_is_obj(patch)) {
        send_error(s, m, SH_RPC_INVALID_PARAMS, "\"patch\" must be an object");
        return;
    }
    yyjson_mut_doc *d = settings_load();
    if (!d) {
        send_error(s, m, SH_RPC_UNPARSABLE, "settings.json cannot be read; it is left as it is until it is fixed");
        return;
    }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d);
    size_t i, n;
    yyjson_val *key, *val;
    yyjson_obj_foreach(patch, i, n, key, val) {
        yyjson_mut_obj_remove_str(root, yyjson_get_str(key));
        if (!yyjson_is_null(val)) yyjson_mut_obj_add(root, yyjson_val_mut_copy(d, key), yyjson_val_mut_copy(d, val));  // null removes
    }
    char *path = settings_path();
    if (!sh_json_write(path, d, true, true)) {
        send_error(s, m, SH_RPC_REFUSED, "could not write settings.json");
    } else {
        send_result(s, m, d, root);
        settings_announce(s);
    }
    free(path);
    yyjson_mut_doc_free(d);
}

static void m_cancel(server *s, sh_rpc_msg *m) {
    if (!s->job || strcmp(s->job->name, param_str(m->params, "job")) != 0) {
        send_error(s, m, SH_RPC_NOT_FOUND, "no such job is running");
        return;
    }
    job_cancel(s->job);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    send_result(s, m, d, yyjson_mut_obj(d));
    yyjson_mut_doc_free(d);
}

typedef struct {
    const char *method;
    void (*fn)(server *s, sh_rpc_msg *m);
} immediate;

static const immediate IMMEDIATE[] = {
    {"skills.read", m_read},
    {"providers.addCustom", m_providers_add},
    {"providers.removeCustom", m_providers_remove},
    {"transfer.export", m_export},
    {"transfer.preview", m_preview},
    {"automation.status", m_automation_status},
    {"automation.set", m_automation_set},
    {"settings.get", m_settings_get},
    {"settings.set", m_settings_set},
    {"jobs.cancel", m_cancel},
};

// returns true when it keeps `m` to answer later
static bool dispatch(server *s, sh_rpc_msg *m) {
    if (strcmp(m->method, "initialize") == 0) {
        m_initialize(s, m);
        return false;
    }
    if (!s->initialized) {
        send_error(s, m, SH_RPC_INVALID_REQUEST, "send initialize first");
        return false;
    }
    if (strcmp(m->method, "shutdown") == 0) return m_shutdown(s, m);
    if (s->shutting_down) {
        send_error(s, m, SH_RPC_REFUSED, "the backend is shutting down");
        return false;
    }
    if (strcmp(m->method, "state.snapshot") == 0 || strcmp(m->method, "providers.list") == 0 || strcmp(m->method, "providers.rescan") == 0)
        return m_from_state(s, m);
    for (size_t i = 0; i < sizeof IMMEDIATE / sizeof IMMEDIATE[0]; ++i) {
        if (strcmp(IMMEDIATE[i].method, m->method) == 0) {
            IMMEDIATE[i].fn(s, m);
            return false;
        }
    }
    bool mutating;
    job_body body;
    const char *kind = job_method_kind(m->method, &mutating, &body);
    if (kind) {
        job_start(s, m, kind, mutating, body);
        return false;
    }
    char *why = sh_strfmt("unknown method: %s", m->method);
    send_error(s, m, SH_RPC_UNKNOWN_METHOD, why);
    free(why);
    return false;
}

static void handle_line(server *s, char *text, size_t len, bool fits) {
    if (!fits) {
        send_error(s, NULL, SH_RPC_PARSE_ERROR, "message longer than 64 MB");
        return;
    }
    if (sh_is_blank(text)) return;
    sh_rpc_msg *m = sh_xcalloc(1, sizeof *m);
    sh_rpc_parse(text, len, m);
    bool kept = false;
    if (m->kind == SH_RPC_BAD) send_error(s, m, m->code, m->message);
    else if (m->kind == SH_RPC_REQUEST) kept = dispatch(s, m);
    // notifications and responses from the window: none are defined, they are ignored
    if (!kept) {
        sh_rpc_msg_free(m);
        free(m);
    }
}

// ---- the loop ----

static void maybe_stop(server *s) {
    if (s->input_closed && !s->job && !s->scan_running) uv_stop(s->loop);
}

static void handle_event(server *s, event *ev) {
    switch (ev->kind) {
    case EV_LINE:
        if (!s->input_closed) handle_line(s, ev->text, ev->len, ev->a != 0);
        free(ev->text);
        break;
    case EV_EOF:
        s->reader_done = true;
        // fall through
    case EV_WRITER_GONE:
        // nobody is listening any more: stop the job at a safe point, keep what it finished, then exit
        s->input_closed = true;
        s->shutting_down = true;
        if (s->job) job_cancel(s->job);
        break;
    case EV_LOG:
        sh_strv_take(&s->pending_logs, ev->text);
        arm_flush(s);
        break;
    case EV_PROGRESS:
        free(s->progress_current);
        s->progress_current = ev->text;
        s->progress_done = ev->a;
        s->progress_total = ev->b;
        s->progress_dirty = true;
        arm_flush(s);
        break;
    case EV_JOB_DONE: {
        char *name = sh_strdup(ev->job->name);
        const bool stopped = ev->job->cancelled;
        job_finish(s, ev->job);
        if (s->shutdown_msg) reply_shutdown(s, stopped ? name : NULL);
        free(name);
        break;
    }
    case EV_SCAN_DONE:
        scan_done(s, ev->data);
        break;
    }
}

static void on_wake(uv_async_t *h) {
    server *s = h->data;
    sh_mutex_lock(s->m);
    event *ev = s->head;
    s->head = s->tail = NULL;
    sh_mutex_unlock(s->m);
    while (ev) {
        event *next = ev->next;
        handle_event(s, ev);
        free(ev);
        ev = next;
    }
    maybe_stop(s);
}

// The protocol gets a private copy of standard output; descriptor 1 then goes to stderr (or nowhere, when there is no
// stderr), so a stray printf, or a child that inherits standard output, can never corrupt the stream.
static int protocol_fd(void) {
#ifdef _WIN32
    _setmode(0, _O_BINARY);
    const int out = _dup(1);
    if (out < 0) return -1;
    _setmode(out, _O_BINARY);
    if (_dup2(2, 1) != 0) {
        const int nul = _open("NUL", _O_WRONLY);
        if (nul >= 0) {
            _dup2(nul, 1);
            _close(nul);
        }
    }
    SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
    return out;
#else
    const int out = fcntl(1, F_DUPFD_CLOEXEC, 3);
    if (out < 0) return -1;
    if (dup2(2, 1) < 0) {
        const int nul = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (nul >= 0) {
            dup2(nul, 1);
            close(nul);
        }
    }
    return out;
#endif
}

static void stderr_log(void *user, const char *line) {
    (void)user;
    fprintf(stderr, "%s\n", line);
}

int sh_serve_stdio(void) {
    sv.out_fd = protocol_fd();
    if (sv.out_fd < 0) {
        fputs("skillhub serve: standard output is not usable\n", stderr);
        return 1;
    }
    // first-call caches, filled before any other thread could race to fill them
    sh_git_program();
    sh_can_make_dir_links();
    sh_flock *lock = NULL;
    if (mutation_lock_try(&lock) && lock) {  // nobody else is changing skills: tidy up after a process that died doing so
        sh_store st;
        sh_store_init(&st, stderr_log, NULL);
        sh_store_recover(&st, true);
        sh_store_free(&st);
    }
    sh_flock_release(lock);

    sv.loop = uv_default_loop();
    sv.m = sh_mutex_new();
    sv.out_m = sh_mutex_new();
    sv.out_cv = sh_cond_new();
    uv_async_init(sv.loop, &sv.wake, on_wake);
    sv.wake.data = &sv;
    uv_timer_init(sv.loop, &sv.flush);
    sv.flush.data = &sv;
    uv_timer_init(sv.loop, &sv.debounce);
    sv.debounce.data = &sv;
    uv_timer_init(sv.loop, &sv.settings_debounce);
    sv.settings_debounce.data = &sv;
    sv.writer = sh_thread_start(writer_main, &sv);
    sv.reader = sh_thread_start(reader_main, &sv);
    if (!sv.writer || !sv.reader) {
        fputs("skillhub serve: could not start its threads\n", stderr);
        return 1;
    }
    yyjson_mut_doc *settings = settings_load();  // what the window reads with settings.get: no settings.changed for it
    if (settings) sv.settings_seen = yyjson_mut_write(settings, 0, NULL);
    yyjson_mut_doc_free(settings);
    request_scan(&sv);  // ready by the time the window asks
    uv_run(sv.loop, UV_RUN_DEFAULT);

    sh_mutex_lock(sv.m);
    sv.closed = true;  // threads still running (a reader blocked on stdin) post nothing any more
    sh_mutex_unlock(sv.m);
    flush_pending(&sv);
    sh_mutex_lock(sv.out_m);
    sv.out_closing = true;
    sh_cond_broadcast(sv.out_cv);
    sh_mutex_unlock(sv.out_m);
    sh_thread_join(sv.writer);  // everything queued is written before the process ends
    if (sv.reader_done) sh_thread_join(sv.reader);  // else it may still be blocked on standard input: the exit ends it
    watch_close_all(&sv);
    scan_result_free(sv.state);
    sv.state = NULL;
    check_map_free(&sv.checks);
    sh_strv_free(&sv.just_updated);
    sh_strv_free(&sv.pending_logs);
    free(sv.progress_current);
    free(sv.settings_seen);
    return 0;
}
