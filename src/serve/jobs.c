// Jobs: anything that changes files or talks to the network (docs/protocol.md, Jobs). One runs at a time, on a thread of
// its own with a store of its own; a job that changes skills holds the mutation lock that the command line uses too.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "serve.h"
#include "sh_engine.h"
#include "sh_fs.h"
#include "sh_path.h"

// ---- reporting from the job's threads ----

static void job_log(void *user, const char *line) {
    job *j = user;
    char *json = log_line_json(line);
    post_event(j->sv, EV_LOG, j, json, strlen(json), 0, 0, NULL);
}

static void job_progress(void *user, size_t done, size_t total, const char *current) {
    job *j = user;
    post_event(j->sv, EV_PROGRESS, j, sh_strdup(current), strlen(current), (long)done, (long)total, NULL);
}

// an engine that jobs.cancel can reach
static sh_engine *engine_for(job *j, sh_store *st) {
    sh_engine *e = sh_engine_new(st);
    sh_engine_on_progress(e, job_progress, j);
    sh_mutex_lock(j->m);
    j->engine = e;
    if (j->cancelled) sh_engine_cancel(e);
    sh_mutex_unlock(j->m);
    return e;
}

static void engine_done(job *j, sh_engine *e) {
    sh_mutex_lock(j->m);
    j->engine = NULL;
    sh_mutex_unlock(j->m);
    sh_engine_free(e);
}

static bool cancelled(job *j) {
    sh_mutex_lock(j->m);
    const bool c = j->cancelled;
    sh_mutex_unlock(j->m);
    return c;
}

void job_cancel(job *j) {
    sh_mutex_lock(j->m);
    j->cancelled = true;
    if (j->engine) sh_engine_cancel(j->engine);
    sh_mutex_unlock(j->m);
}

static void add_result(job *j, const char *name, bool ok, const char *error) {
    yyjson_mut_val *o = yyjson_mut_obj(j->out);
    yyjson_mut_obj_add_strcpy(j->out, o, "name", name);
    yyjson_mut_obj_add_bool(j->out, o, "ok", ok);
    if (error && *error) yyjson_mut_obj_add_strcpy(j->out, o, "error", error);
    yyjson_mut_arr_append(j->results, o);
}

static void sum(job *j, const char *key, long long n) { yyjson_mut_obj_add_int(j->out, j->summary, key, n); }

static void fail(job *j, const char *why) {
    j->ok = false;
    free(j->error);
    j->error = sh_strdup(why ? why : "");
}

// ---- check and update ----

static const char *status_word(sh_update u) {
    switch (u) {
    case SH_UPDATE_CURRENT: return "current";
    case SH_UPDATE_AVAILABLE: return "available";
    case SH_UPDATE_ERROR: return "error";
    default: return NULL;  // not checked (the job was cancelled first)
    }
}

static bool wanted(const sh_strv *names, const char *name) { return names->len == 0 || sh_strv_contains(names, name); }

// checks `names` (all when empty); remembers and reports each result
static sh_engine *check_skills(job *j, sh_store *st, sh_skills *sk, const sh_strv *names, int *available, int *errors) {
    sh_engine *e = engine_for(j, st);
    *available = sh_engine_check(e, sk, names);
    *errors = 0;
    for (size_t i = 0; i < sk->len; ++i) {
        const sh_skill *s = &sk->items[i];
        const char *status = status_word(s->update);
        if (!wanted(names, s->name) || !sh_source_tracked(&s->src) || !status) continue;
        check_map_add(&j->checks, s->name, status, s->note, s->src.hash, s->new_hash);
        *errors += s->update == SH_UPDATE_ERROR;
    }
    return e;
}

static void body_check(job *j, sh_store *st) {
    sh_strv names = SH_STRV_INIT;
    param_strv(j->params, "names", &names);
    sh_skills sk;
    sh_store_scan(st, &sk);
    int available, errors;
    sh_engine *e = check_skills(j, st, &sk, &names, &available, &errors);
    engine_done(j, e);
    for (size_t i = 0; i < j->checks.len; ++i) {
        const check_entry *c = &j->checks.items[i];
        yyjson_mut_val *o = yyjson_mut_obj(j->out);
        yyjson_mut_obj_add_strcpy(j->out, o, "name", c->name);
        yyjson_mut_obj_add_bool(j->out, o, "ok", strcmp(c->status, "error") != 0);
        yyjson_mut_obj_add_strcpy(j->out, o, "status", c->status);
        if (*c->note) yyjson_mut_obj_add_strcpy(j->out, o, "note", c->note);
        yyjson_mut_arr_append(j->results, o);
    }
    sum(j, "available", available);
    sum(j, "errors", errors);
    sum(j, "checked", (long long)j->checks.len);
    j->rescan = true;
    sh_skills_free(&sk);
    sh_strv_free(&names);
}

static void body_update(job *j, sh_store *st) {
    sh_strv names = SH_STRV_INIT;
    param_strv(j->params, "names", &names);
    sh_skills sk;
    sh_store_scan(st, &sk);
    int available, errors;
    sh_engine *e = check_skills(j, st, &sk, &names, &available, &errors);
    sh_skill **todo = sh_xcalloc(sk.len + 1, sizeof *todo);
    size_t n = 0;
    for (size_t i = 0; i < sk.len; ++i)
        if (wanted(&names, sk.items[i].name) && sk.items[i].update == SH_UPDATE_AVAILABLE) todo[n++] = &sk.items[i];
    bool *done = sh_xcalloc(n + 1, sizeof *done);
    const int updated = n && !cancelled(j) ? sh_engine_update(e, todo, n, done) : 0;
    engine_done(j, e);
    const bool stopped = cancelled(j);
    int failed = 0, skipped = 0;
    for (size_t i = 0; i < n; ++i) {
        if (done[i]) {
            // the engine moved the skill's lock entry on to the new version
            check_map_add(&j->checks, todo[i]->name, "current", "", todo[i]->src.hash, todo[i]->src.hash);
            sh_strv_push(&j->updated, todo[i]->name);
            add_result(j, todo[i]->name, true, NULL);
        } else if (stopped) {
            ++skipped;
            add_result(j, todo[i]->name, false, "cancelled");
        } else {
            ++failed;
            add_result(j, todo[i]->name, false, "not updated (see the activity log)");
        }
    }
    sum(j, "updated", updated);
    sum(j, "failed", failed);
    sum(j, "skipped", skipped);
    if (failed) j->ok = false;
    j->rescan = true;
    free(done);
    free(todo);
    sh_skills_free(&sk);
    sh_strv_free(&names);
}

// ---- adding skills ----

static void body_discover(job *j, sh_store *st) {
    const char *input = param_str(j->params, "input");
    sh_engine *e = engine_for(j, st);
    sh_discovery found;
    sh_engine_discover(e, &input, 1, &found);
    engine_done(j, e);
    if (found.cands.len == 0) {
        fail(j, found.err);
    } else {
        yyjson_mut_val *list = yyjson_mut_arr(j->out);
        for (size_t i = 0; i < found.cands.len; ++i) {
            const sh_candidate *c = &found.cands.items[i];
            char *dest = sh_path_join(st->central, c->name);
            yyjson_mut_val *o = yyjson_mut_obj(j->out);
            yyjson_mut_obj_add_strcpy(j->out, o, "name", c->name);
            yyjson_mut_obj_add_strcpy(j->out, o, "description", c->description);
            yyjson_mut_obj_add_strcpy(j->out, o, "dir", c->dir);
            yyjson_mut_obj_add_bool(j->out, o, "installed", sh_exists(dest));
            yyjson_mut_arr_append(list, o);
            free(dest);
        }
        yyjson_mut_obj_add_val(j->out, j->extra, "candidates", list);
        yyjson_mut_obj_add_strcpy(j->out, j->extra, "source", found.cands.items[0].slug);
        yyjson_mut_obj_add_strcpy(j->out, j->extra, "host", found.host);
        yyjson_mut_obj_add_strcpy(j->out, j->extra, "sourceType", found.type);
    }
    sh_discovery_free(&found);
}

static void body_install(job *j, sh_store *st) {
    const char *source = param_str(j->params, "source");
    const bool link = strcmp(param_str(j->params, "mode"), "link") == 0;
    sh_strv want = SH_STRV_INIT;
    param_strv(j->params, "skills", &want);
    sh_engine *e = engine_for(j, st);
    sh_discovery found;
    sh_engine_discover(e, &source, 1, &found);
    if (found.cands.len == 0) {
        fail(j, found.err);
    } else {
        const sh_candidate **pick = sh_xcalloc(found.cands.len + 1, sizeof *pick);
        size_t n = 0;
        for (size_t i = 0; i < found.cands.len; ++i)
            if (sh_strv_contains(&want, found.cands.items[i].name)) pick[n++] = &found.cands.items[i];
        bool *done = sh_xcalloc(n + 1, sizeof *done);
        const int installed = n && !cancelled(j) ? sh_engine_install(e, pick, n, link, done) : 0;
        for (size_t i = 0; i < n; ++i) add_result(j, pick[i]->name, done[i], done[i] ? NULL : cancelled(j) ? "cancelled" : "not installed (see the activity log)");
        for (size_t i = 0; i < want.len; ++i) {
            bool offered = false;
            for (size_t k = 0; k < n && !offered; ++k) offered = strcmp(pick[k]->name, want.items[i]) == 0;
            if (!offered) {
                char *why = sh_strfmt("%s has no skill of that name", source);
                add_result(j, want.items[i], false, why);
                free(why);
            }
        }
        sum(j, "installed", installed);
        sum(j, "failed", (long long)want.len - installed);
        if (installed < (int)want.len) j->ok = false;
        free(done);
        free(pick);
    }
    engine_done(j, e);
    j->rescan = true;
    sh_discovery_free(&found);
    sh_strv_free(&want);
}

// ---- single skills and housekeeping ----

static void body_remove(job *j, sh_store *st) {
    const char *name = param_str(j->params, "name");
    const bool ok = sh_store_remove_skill(st, name);
    add_result(j, name, ok, ok ? NULL : "could not remove it");
    sum(j, "removed", ok);
    if (!ok) fail(j, "could not remove it");
    j->rescan = true;
}

static void body_track(job *j, sh_store *st) {
    const char *name = param_str(j->params, "name");
    char *err = NULL;
    const bool ok = sh_store_track(st, name, param_str(j->params, "source"), param_str(j->params, "path"), &err);
    add_result(j, name, ok, err);
    if (!ok) fail(j, err);
    free(err);
    j->rescan = true;
}

static void body_adopt(job *j, sh_store *st) {
    sum(j, "changes", sh_store_adopt(st));
    j->rescan = true;
}

static void body_relink(job *j, sh_store *st) {
    sum(j, "changes", sh_store_relink(st));
    j->rescan = true;
}

// switching an agent on links the skills into it; switching it off leaves what is there untouched (as 1.x)
static void body_set_managed(job *j, sh_store *st) {
    const char *id = param_str(j->params, "id");
    const bool on = param_bool(j->params, "on", false);
    char *err = NULL;
    if (!sh_store_set_provider_managed(st, id, on, &err)) {
        fail(j, err);
    } else {
        sh_store_sync_watcher(st);
        sum(j, "changes", on ? sh_store_adopt(st) : 0);
    }
    free(err);
    j->rescan = true;
}

static void body_import(job *j, sh_store *st) {
    sh_strv names = SH_STRV_INIT;
    param_strv(j->params, "names", &names);
    char *why = NULL;
    sh_import_items items;
    sh_store_read_export(st, param_str(j->params, "path"), &items, &why);
    if (items.len == 0) {
        fail(j, why ? why : "nothing to import in that file");
    } else {
        const sh_import_item **todo = sh_xcalloc(items.len + 1, sizeof *todo);
        size_t n = 0;
        for (size_t i = 0; i < items.len; ++i)
            if (sh_strv_contains(&names, items.items[i].name)) todo[n++] = &items.items[i];
        int ok = 0, failed = 0;
        sh_store_import_items(st, todo, n, &ok, &failed);
        sh_store_relink(st);
        sum(j, "installed", ok);
        sum(j, "failed", failed);
        if (failed) j->ok = false;
        free(todo);
    }
    free(why);
    sh_import_items_free(&items);
    sh_strv_free(&names);
    j->rescan = true;
}

// ---- starting and finishing ----

typedef struct {
    const char *method, *kind;
    bool mutating;
    job_body body;
    const char *required[3];  // parameters that must be there
} job_method;

static const job_method METHODS[] = {
    {"skills.check", "check", false, body_check, {NULL}},
    {"skills.update", "update", true, body_update, {NULL}},
    {"skills.install", "install", true, body_install, {"source", "skills", NULL}},
    {"skills.remove", "remove", true, body_remove, {"name", NULL}},
    {"skills.track", "track", true, body_track, {"name", "source", NULL}},
    {"sources.discover", "discover", false, body_discover, {"input", NULL}},
    {"adopt", "adopt", true, body_adopt, {NULL}},
    {"relink", "relink", true, body_relink, {NULL}},
    {"providers.setManaged", "setManaged", true, body_set_managed, {"id", "on", NULL}},
    {"transfer.import", "import", true, body_import, {"path", "names", NULL}},
};

const char *job_method_kind(const char *method, bool *mutating, job_body *body) {
    for (size_t i = 0; i < sizeof METHODS / sizeof METHODS[0]; ++i) {
        if (strcmp(METHODS[i].method, method) != 0) continue;
        *mutating = METHODS[i].mutating;
        *body = METHODS[i].body;
        return METHODS[i].kind;
    }
    return NULL;
}

static const char *missing_param(const char *kind, yyjson_val *params) {
    for (size_t i = 0; i < sizeof METHODS / sizeof METHODS[0]; ++i) {
        if (strcmp(METHODS[i].kind, kind) != 0) continue;
        for (const char *const *r = METHODS[i].required; *r; ++r) {
            yyjson_val *v = params && yyjson_is_obj(params) ? yyjson_obj_get(params, *r) : NULL;
            if (!v || (yyjson_is_str(v) && !*yyjson_get_str(v)) || (yyjson_is_arr(v) && yyjson_arr_size(v) == 0)) return *r;
        }
    }
    return NULL;
}

static void job_main(void *arg) {
    job *j = arg;
    sh_store st;
    sh_store_init(&st, job_log, j);
    if (j->mutating) sh_store_recover(&st, false);  // the lock is ours: a dead process's half-done swap is put right first
    j->body(j, &st);
    if (cancelled(j)) j->ok = false;
    check_map_sort(&j->checks);
    sh_store_free(&st);
    sh_flock_release(j->lock);  // before the window hears the job is done, so its next job can take the lock
    j->lock = NULL;
    post_event(j->sv, EV_JOB_DONE, j, NULL, 0, 0, 0, NULL);
}

static void job_free(job *j) {
    yyjson_doc_free(j->params_doc);
    yyjson_mut_doc_free(j->out);
    free(j->error);
    check_map_free(&j->checks);
    sh_strv_free(&j->updated);
    sh_mutex_free(j->m);
    free(j);
}

// why a job cannot start now, or NULL; *code is the error to answer with
static const char *refusal(server *sv, sh_rpc_msg *m, const char *kind, int *code, char **owned) {
    *owned = NULL;
    *code = SH_RPC_REFUSED;
    if (sv->shutting_down) return "the backend is shutting down";
    const char *missing = missing_param(kind, m->params);
    if (missing) {
        *code = SH_RPC_INVALID_PARAMS;
        return *owned = sh_strfmt("missing parameter \"%s\"", missing);
    }
    *code = SH_RPC_BUSY;
    return sv->job ? "another job is running" : NULL;
}

void job_start(server *sv, sh_rpc_msg *m, const char *kind, bool mutating, job_body body) {
    int code;
    char *owned;
    const char *why = refusal(sv, m, kind, &code, &owned);
    sh_flock *lock = NULL;
    if (!why && mutating && !mutation_lock_try(&lock)) {
        code = SH_RPC_LOCKED;
        why = "another Skill Hub is changing skills (a command line run, or the daily update)";
    }
    if (why) {
        send_error(sv, m, code, why);
        free(owned);
        return;
    }
    job *j = sh_xcalloc(1, sizeof *j);
    j->sv = sv;
    snprintf(j->name, sizeof j->name, "j%ld", ++sv->next_job);
    j->kind = kind;
    j->mutating = mutating;
    j->lock = lock;
    j->body = body;
    j->m = sh_mutex_new();
    j->out = yyjson_mut_doc_new(NULL);
    j->summary = yyjson_mut_obj(j->out);
    j->results = yyjson_mut_arr(j->out);
    j->extra = yyjson_mut_obj(j->out);
    j->ok = true;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *r = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, r, "job", j->name);
    send_result(sv, m, d, r);  // before any of the job's own events
    yyjson_mut_doc_free(d);
    j->params_doc = m->doc;  // the job keeps the request's parameters
    j->params = m->params;
    m->doc = NULL;
    sv->job = j;
    j->thread = sh_thread_start(job_main, j);
    if (!j->thread) {
        fail(j, "could not start a thread for the job");
        sh_flock_release(j->lock);
        j->lock = NULL;
        post_event(sv, EV_JOB_DONE, j, NULL, 0, 0, 0, NULL);
    }
}

void job_finish(server *sv, job *j) {
    sh_thread_join(j->thread);
    flush_pending(sv);  // the job's last progress and log lines come before its end
    check_map_merge(&sv->checks, &j->checks);
    for (size_t i = 0; i < j->updated.len; ++i)
        if (!sh_strv_contains(&sv->just_updated, j->updated.items[i])) sh_strv_push(&sv->just_updated, j->updated.items[i]);
    yyjson_mut_doc *d = j->out;
    yyjson_mut_val *p = j->extra;  // kind-specific fields are there already
    yyjson_mut_obj_add_str(d, p, "job", j->name);
    yyjson_mut_obj_add_str(d, p, "kind", j->kind);
    yyjson_mut_obj_add_bool(d, p, "ok", j->ok);
    if (cancelled(j)) yyjson_mut_obj_add_bool(d, p, "cancelled", true);
    if (j->error) yyjson_mut_obj_add_strcpy(d, p, "error", j->error);
    yyjson_mut_obj_add_val(d, p, "summary", j->summary);
    yyjson_mut_obj_add_val(d, p, "results", j->results);
    send_notification(sv, d, "event.jobDone", p);
    sv->job = NULL;
    if (j->rescan) {
        sv->quiet_until = uv_now(sv->loop) + 500;  // the watchers still report the job's own changes for a moment
        request_scan(sv);
    }
    job_free(j);
}
