// Inside `skillhub serve` (docs/protocol.md). One libuv loop thread owns all protocol state; every other thread (the
// stdin reader, the stdout writer, each job, the scanner) talks to it only by posting events. Jobs and scans each use a
// sh_store of their own, so no store is shared between threads.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uv.h>

#include "sh_fs.h"
#include "sh_rpc.h"
#include "sh_store.h"
#include "sh_str.h"
#include "sh_thread.h"
#include "yyjson.h"

typedef struct server server;
typedef struct job job;

// ---- events: from any thread to the loop ----

typedef enum {
    EV_LINE,         // a line arrived on stdin (text NULL: it was too long)
    EV_EOF,          // stdin closed
    EV_WRITER_GONE,  // stdout cannot be written any more
    EV_LOG,          // a job's log line (text: the line object as JSON)
    EV_PROGRESS,     // a job's progress (a done, b total, text current)
    EV_JOB_DONE,     // a job ended
    EV_SCAN_DONE,    // a scan finished (data: a scan_result)
} event_kind;

typedef struct event {
    event_kind kind;
    char *text;
    size_t len;
    job *job;
    long a, b;
    void *data;
    struct event *next;
} event;

void post_event(server *sv, event_kind kind, job *j, char *text, size_t len, long a, long b, void *data);

// ---- what the window sees (state.c) ----

// one check result, remembered until the lock entry moves on (docs/protocol.md, state.snapshot)
typedef struct {
    char *name;
    char *status;     // "current", "available" or "error"
    char *note;
    char *lock_hash;  // the lock entry's hash when it was checked
    char *new_hash;   // what upstream had
} check_entry;

typedef struct {
    check_entry *items;
    size_t len, cap;
} check_map;

// A job adds results in any order and sorts once; the server's map is always sorted, and a job's results are merged in.
void check_map_add(check_map *m, const char *name, const char *status, const char *note, const char *lock_hash, const char *new_hash);
void check_map_sort(check_map *m);  // by name; of a name added twice, the later result stays
void check_map_merge(check_map *dst, check_map *src);  // both sorted; src's results win, src is emptied
void check_map_remove(check_map *m, const char *name);
void check_map_free(check_map *m);
check_map check_map_copy(const check_map *m);

typedef struct {
    char *name;
    char *json;  // the <Skill> object, compact JSON
} row;

typedef struct {
    row *rows;           // in the order the shared folder lists them
    size_t n;
    char *agents;        // JSON array of <AgentTile>
    char *overflow;      // JSON object, or "null"
    char *providers;     // JSON array of <Provider>
    char *counts;        // JSON object
    int managed, known;  // agents managed and known
    sh_strv watch_dirs;  // folders the watchers should follow
    sh_strv watch_files; // file names that matter in those folders ("" = any)
    sh_strv stale;       // check entries that no longer match their lock entry
} scan_result;

// Runs on the scanner thread: the whole state from a fresh store, with the check results applied.
scan_result *scan_build(const check_map *checks, const sh_strv *just_updated);
void scan_result_free(scan_result *r);
// The state.delta params between two scans, or NULL when nothing changed. `d` owns the result.
yyjson_mut_val *scan_delta(yyjson_mut_doc *d, const scan_result *before, const scan_result *after, long base_rev, long rev);
// One page of state.snapshot; page 0 also carries agents, overflow, providers, projects and counts.
yyjson_mut_val *scan_page(yyjson_mut_doc *d, const scan_result *r, long rev, size_t page, size_t page_size);
char *log_line_json(const char *text);  // a job's log line as {time, level, text}

// ---- jobs (jobs.c) ----

typedef void (*job_body)(job *j, sh_store *st);

struct job {
    server *sv;
    char name[24];      // "j7"
    const char *kind;   // "check", "update", ...
    bool mutating;
    sh_flock *lock;     // the mutation lock, held for the whole job
    yyjson_doc *params_doc;
    yyjson_val *params;
    job_body body;
    sh_thread *thread;
    sh_mutex *m;        // guards engine and cancelled
    struct sh_engine *engine;
    bool cancelled;
    // filled by the job thread
    yyjson_mut_doc *out;
    yyjson_mut_val *summary, *results, *extra;  // extra: kind-specific fields of jobDone
    bool ok;
    char *error;
    check_map checks;       // check results to remember
    sh_strv updated;        // skills this job updated
    bool rescan;            // the shared folder may have changed
};

// Starts a job for request `m` (replies {job} or an error). `mutating` jobs need the mutation lock.
void job_start(server *sv, sh_rpc_msg *m, const char *kind, bool mutating, job_body body);
void job_cancel(job *j);
void job_finish(server *sv, job *j);  // on the loop thread, after EV_JOB_DONE: reports and frees it
const char *job_method_kind(const char *method, bool *mutating, job_body *body);  // NULL: not a job

// ---- the server (serve.c) ----

typedef struct waiting waiting;

struct server {
    uv_loop_t *loop;
    uv_async_t wake;
    uv_timer_t flush;     // progress and log lines go out merged, at most every 50 ms
    uv_timer_t debounce;  // a rescan 300 ms after the last outside change
    uv_timer_t settings_debounce;
    sh_mutex *m;          // guards the event queue
    event *head, *tail;

    // output, written by the writer thread
    int out_fd;
    sh_mutex *out_m;
    sh_cond *out_cv;
    struct out_line *out_head, *out_tail;
    size_t out_bytes;
    bool out_closing, out_failed;
    sh_thread *reader, *writer;
    bool reader_done;  // it saw the end of standard input and returned
    bool closed;       // no more events are taken (guarded by m)

    bool initialized, shutting_down, input_closed;
    sh_rpc_msg *shutdown_msg;  // answered once the running job has stopped

    // state
    scan_result *state;
    long rev;
    check_map checks;
    sh_strv just_updated;
    bool scan_running, scan_again, resync_pending;
    sh_thread *scanner;
    waiting *waiting;      // requests answered after the next scan

    // jobs
    job *job;
    long next_job;
    bool progress_dirty;   // pending progress, sent merged
    long progress_done, progress_total;
    char *progress_current;
    sh_strv pending_logs;  // JSON objects
    char *settings_seen;   // settings.json as last announced

    // watchers (watch.c)
    struct watch **watches;  // each allocated on its own: libuv keeps pointers to the handles
    size_t n_watches;
    sh_strv watch_files;
    int64_t quiet_until;   // our own job's changes still arriving
};

void send_line(server *sv, char *line);  // takes it; responses and notifications leave in this order
void send_result(server *sv, sh_rpc_msg *m, yyjson_mut_doc *d, yyjson_mut_val *result);
void send_error(server *sv, sh_rpc_msg *m, int code, const char *message);
void send_notification(server *sv, yyjson_mut_doc *d, const char *method, yyjson_mut_val *params);
void request_scan(server *sv);
// false when another process holds the mutation lock; *held stays NULL when there is no state folder to lock in
bool mutation_lock_try(sh_flock **held);
void loop_log(void *user, const char *line);  // a store's log callback on the loop thread: user is the server
void settings_announce(server *sv);           // settings.changed, when settings.json differs from what was announced
void wait_for_scan(server *sv, sh_rpc_msg *m);  // answer `m` after the next scan
void flush_pending(server *sv);                 // progress and log lines, now
size_t output_queued(server *sv);

// ---- watchers (watch.c) ----

void watch_sync(server *sv, const sh_strv *dirs, const sh_strv *files);
void watch_close_all(server *sv);

// ---- small helpers ----

const char *param_str(yyjson_val *params, const char *key);  // "" when missing
bool param_bool(yyjson_val *params, const char *key, bool fallback);
void param_strv(yyjson_val *params, const char *key, sh_strv *out);  // an array of strings
