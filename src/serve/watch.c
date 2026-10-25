// File watchers (docs/protocol.md, File watching): the shared folder, every managed agent folder, the folders of the lock
// files and the config folder. An event only marks the state as dirty; one rescan follows 300 ms after the last one.
// Folders are watched, never files: lock files are replaced by a rename, and a watch on the old file would go silent.
#include <stdlib.h>
#include <string.h>

#include "serve.h"
#include "sh_fs.h"
#include "sh_path.h"

struct watch {
    uv_fs_event_t h;
    server *sv;
    char *dir;
    char *file;  // the one name that matters in that folder; "" = any
    bool config; // the config folder: settings.json and providers.json
    bool keep;
};

static void on_debounce(uv_timer_t *t) { request_scan(t->data); }

static void on_settings(uv_timer_t *t) { settings_announce(t->data); }

static void on_change(uv_fs_event_t *h, const char *filename, int events, int status) {
    struct watch *w = h->data;
    server *sv = w->sv;
    (void)events;
    const char *name = filename ? sh_path_name(filename) : "";
    if (w->config) {
        if (strcmp(name, "settings.json") == 0) uv_timer_start(&sv->settings_debounce, on_settings, 200, 0);
        if (strcmp(name, "providers.json") != 0 && status >= 0) return;
    } else if (status >= 0 && *w->file && *name && strcmp(name, w->file) != 0) {
        return;
    }
    // changes the running job makes are covered by the rescan that follows it
    if ((sv->job && sv->job->mutating) || uv_now(sv->loop) < (uint64_t)sv->quiet_until) return;
    uv_timer_start(&sv->debounce, on_debounce, 300, 0);
}

static void on_closed(uv_handle_t *h) {
    struct watch *w = h->data;
    free(w->dir);
    free(w->file);
    free(w);
}

static void drop(struct watch *w) {
    uv_fs_event_stop(&w->h);
    uv_close((uv_handle_t *)&w->h, on_closed);
}

static struct watch *find(server *sv, const char *dir) {
    for (size_t i = 0; i < sv->n_watches; ++i)
        if (strcmp(sv->watches[i]->dir, dir) == 0) return sv->watches[i];
    return NULL;
}

// Watches exactly `dirs` (files[i]: the name that matters in dirs[i]). Folders that do not exist yet are picked up by a
// later sync, after the scan that follows the change creating them.
void watch_sync(server *sv, const sh_strv *dirs, const sh_strv *files) {
    for (size_t i = 0; i < sv->n_watches; ++i) sv->watches[i]->keep = false;
    char *config = sh_config_dir();
    for (size_t i = 0; i < dirs->len; ++i) {
        struct watch *w = find(sv, dirs->items[i]);
        if (w) {
            w->keep = true;
            free(w->file);
            w->file = sh_strdup(files->items[i]);
            continue;
        }
        if (!sh_is_dir(dirs->items[i])) continue;
        w = sh_xcalloc(1, sizeof *w);
        w->sv = sv;
        w->dir = sh_strdup(dirs->items[i]);
        w->file = sh_strdup(files->items[i]);
        w->config = strcmp(w->dir, config) == 0;
        w->keep = true;
        w->h.data = w;
        uv_fs_event_init(sv->loop, &w->h);
        if (uv_fs_event_start(&w->h, on_change, w->dir, 0) != 0) {
            uv_close((uv_handle_t *)&w->h, on_closed);
            continue;
        }
        sv->watches = sh_xrealloc(sv->watches, (sv->n_watches + 1) * sizeof *sv->watches);
        sv->watches[sv->n_watches++] = w;
    }
    free(config);
    size_t kept = 0;
    for (size_t i = 0; i < sv->n_watches; ++i) {
        if (sv->watches[i]->keep) sv->watches[kept++] = sv->watches[i];
        else drop(sv->watches[i]);
    }
    sv->n_watches = kept;
    sv->debounce.data = sv;
    sv->settings_debounce.data = sv;
}

void watch_close_all(server *sv) {
    for (size_t i = 0; i < sv->n_watches; ++i) drop(sv->watches[i]);
    free(sv->watches);
    sv->watches = NULL;
    sv->n_watches = 0;
}
