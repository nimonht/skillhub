// `skillhub watch --run`: the stray watcher where the system has no file-change trigger of its own (Windows, whose Task
// Scheduler starts this at logon). It watches the agents' folders, and three seconds after the last change does what
// `skillhub adopt --notify` does. It reads the list of folders again after every run, so it follows agents as they come.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "sh_fs.h"
#include "sh_path.h"
#include "sh_serve.h"
#include "sh_store.h"
#include "sh_thread.h"

#define DEBOUNCE_MS 3000

typedef struct {
    uv_loop_t *loop;
    uv_timer_t debounce;
    uv_fs_event_t **watches;  // each its own allocation: a handle must stay until it has closed
    size_t n;
} resident;

static void pass(uv_timer_t *t);

static void log_line(void *user, const char *line) {
    (void)user;
    fprintf(stderr, "%s\n", line);
}

static void on_change(uv_fs_event_t *h, const char *filename, int events, int status) {
    resident *r = h->data;
    (void)filename;
    (void)events;
    (void)status;
    uv_timer_start(&r->debounce, pass, DEBOUNCE_MS, 0);
}

static void on_closed(uv_handle_t *h) { free(h); }

static void arm(resident *r, const sh_store *st) {
    for (size_t i = 0; i < r->n; ++i) {
        uv_fs_event_stop(r->watches[i]);
        uv_close((uv_handle_t *)r->watches[i], on_closed);
    }
    free(r->watches);
    sh_strv dirs = SH_STRV_INIT;
    sh_store_watch_dirs(st, &dirs);
    char *config = sh_config_dir();
    sh_strv_push(&dirs, config);  // providers.json: an agent was added
    free(config);
    r->watches = sh_xcalloc(dirs.len + 1, sizeof *r->watches);
    r->n = 0;
    for (size_t i = 0; i < dirs.len; ++i) {
        uv_fs_event_t *w = sh_xcalloc(1, sizeof *w);
        sh_mkpath(dirs.items[i]);
        uv_fs_event_init(r->loop, w);
        w->data = r;
        if (uv_fs_event_start(w, on_change, dirs.items[i], 0) == 0) r->watches[r->n++] = w;
        else uv_close((uv_handle_t *)w, on_closed);
    }
    sh_strv_free(&dirs);
}

static void pass(uv_timer_t *t) {
    resident *r = t->loop->data;
    sh_store st;
    sh_store_init(&st, log_line, NULL);
    char *state = sh_state_dir();
    char *lock_path = sh_path_join(state, "mutate.lock");
    sh_flock *lock = *state ? sh_flock_take(lock_path, 0) : NULL;
    if (lock || !*state) {
        const int n = sh_store_adopt(&st);
        if (n) {
            char *body = sh_strfmt("Tidied %d stray skill change(s) into ~/.agents/skills", n);
            sh_notify("Skill Hub", body);
            free(body);
        }
        sh_flock_release(lock);
        arm(r, &st);
    } else {
        uv_timer_start(t, pass, DEBOUNCE_MS, 0);  // another Skill Hub is at work: look again when it may be done
    }
    free(lock_path);
    free(state);
    sh_store_free(&st);
}

int sh_watch_run(void) {
    resident r;
    memset(&r, 0, sizeof r);
    r.loop = uv_default_loop();
    r.loop->data = &r;
    uv_timer_init(r.loop, &r.debounce);
    sh_store st;
    sh_store_init(&st, log_line, NULL);
    sh_mkpath(st.central);
    arm(&r, &st);
    uv_timer_start(&r.debounce, pass, 0, 0);  // what piled up while nothing was watching
    sh_store_free(&st);
    return uv_run(r.loop, UV_RUN_DEFAULT);
}
