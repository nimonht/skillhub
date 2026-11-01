#include "sh_storage.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sh_fs.h"
#include "sh_path.h"
#include "sh_state.h"
#include "sh_time.h"

#define CACHE_MAX_MB 1024  // settings: storage.cacheMaxMB, 0 for no limit
#define BACKUP_DAYS 90     // settings: storage.backupDays, 0 to keep them for good

static const char *const LOOSE[] = {"_removed", "_dupes", ".parked-replaced"};

// the folders of the backup root that hold no versions of one skill
static bool holds_versions(const char *name) { return name[0] != '_' && name[0] != '.'; }

static int64_t bytes_of(const char *path) {
    const int64_t n = sh_tree_bytes(path);
    return n > 0 ? n : 0;
}

static void remove_backup(const char *path) {
    char *sidecar = sh_strfmt("%s.json", path);
    sh_remove_tree(path);
    sh_remove_file(sidecar);
    free(sidecar);
}

// every backup in one folder below the root
static void remove_all_in(const sh_store *st, const char *folder) {
    char *dir = sh_path_join(st->backup, folder);
    sh_strv names = SH_STRV_INIT;
    sh_list_dir(dir, SH_LIST_DIRS, &names);
    for (size_t i = 0; i < names.len; ++i) {
        char *path = sh_path_join(dir, names.items[i]);
        remove_backup(path);
        free(path);
    }
    sh_strv_free(&names);
    free(dir);
}

// a clone in use by another process stays, for now
static bool remove_clone(const char *clone) {
    char *lock_path = sh_strfmt("%s.lock", clone);
    sh_flock *lock = sh_flock_take(lock_path, 0);
    free(lock_path);
    if (!lock) return false;
    sh_remove_tree(clone);
    sh_flock_release(lock);
    return true;
}

void sh_storage_measure(const sh_store *st, sh_storage *out) {
    memset(out, 0, sizeof *out);
    sh_strv clones = SH_STRV_INIT, skills = SH_STRV_INIT;
    sh_cache_clones(st, &clones);
    for (size_t i = 0; i < clones.len; ++i) out->cache += bytes_of(clones.items[i]);
    sh_strv_free(&clones);
    sh_list_dir(st->backup, SH_LIST_DIRS, &skills);
    for (size_t i = 0; i < skills.len; ++i) {
        if (!holds_versions(skills.items[i])) continue;
        char *dir = sh_path_join(st->backup, skills.items[i]);
        sh_strv versions = SH_STRV_INIT;
        sh_list_dir(dir, SH_LIST_DIRS | SH_SORT_NAME, &versions);
        for (size_t k = 0; k + 1 < versions.len; ++k) {
            char *path = sh_path_join(dir, versions.items[k]);
            out->versions += bytes_of(path);
            free(path);
        }
        sh_strv_free(&versions);
        free(dir);
    }
    sh_strv_free(&skills);
    for (size_t i = 0; i < sizeof LOOSE / sizeof LOOSE[0]; ++i) {
        char *dir = sh_path_join(st->backup, LOOSE[i]);
        out->removed += bytes_of(dir);
        free(dir);
    }
    char *conflicts = sh_path_join(st->backup, "_conflicts");
    out->conflicts = bytes_of(conflicts);
    free(conflicts);
}

static int64_t total_of(const sh_storage *s) { return s->cache + s->versions + s->removed + s->conflicts; }

int64_t sh_storage_clean(sh_store *st, int what) {
    sh_storage before, after;
    sh_storage_measure(st, &before);
    if (what & SH_CLEAN_CACHE) {
        sh_strv clones = SH_STRV_INIT;
        sh_cache_clones(st, &clones);
        for (size_t i = 0; i < clones.len; ++i) remove_clone(clones.items[i]);
        sh_strv_free(&clones);
    }
    if (what & SH_CLEAN_VERSIONS) {
        sh_strv skills = SH_STRV_INIT;
        sh_list_dir(st->backup, SH_LIST_DIRS, &skills);
        for (size_t i = 0; i < skills.len; ++i)
            if (holds_versions(skills.items[i])) sh_backups_prune(st->backup, skills.items[i], 1);
        sh_strv_free(&skills);
    }
    if (what & SH_CLEAN_REMOVED)
        for (size_t i = 0; i < sizeof LOOSE / sizeof LOOSE[0]; ++i) remove_all_in(st, LOOSE[i]);
    if (what & SH_CLEAN_CONFLICTS) remove_all_in(st, "_conflicts");
    sh_storage_measure(st, &after);
    const int64_t freed = total_of(&before) - total_of(&after);
    return freed > 0 ? freed : 0;
}

// ---- the age of a backup ----

static bool digits(const char *s, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

static int number(const char *s, int n) {
    int v = 0;
    for (int i = 0; i < n; ++i) v = v * 10 + (s[i] - '0');
    return v;
}

// The time in a name that ends in <yyyyMMdd-HHmmss>, perhaps with a counter after it; -1 when it holds none. The folder's own
// time will not do: a move keeps it, so a skill last changed a year ago and removed today would count as a year old.
static int64_t stamp_ms(const char *name) {
    const size_t len = strlen(name);
    if (len < 16) return -1;
    for (size_t i = len - 15; i >= 1; --i) {
        const char *s = name + i, *rest = s + 15;
        if (s[-1] != '-' || !digits(s, 8) || s[8] != '-' || !digits(s + 9, 6)) continue;
        if (*rest && (*rest != '-' || !rest[1] || strspn(rest + 1, "0123456789") != strlen(rest + 1))) continue;
        struct tm t;
        memset(&t, 0, sizeof t);
        t.tm_year = number(s, 4) - 1900;
        t.tm_mon = number(s + 4, 2) - 1;
        t.tm_mday = number(s + 6, 2);
        t.tm_hour = number(s + 9, 2);
        t.tm_min = number(s + 11, 2);
        t.tm_sec = number(s + 13, 2);
        t.tm_isdst = -1;
        const time_t when = mktime(&t);
        return when == (time_t)-1 ? -1 : (int64_t)when * 1000;
    }
    return -1;
}

// a backup that cannot be dated stays: nothing goes on a guess
static void expire_loose(sh_store *st) {
    const long long days = sh_setting_int("storage", "backupDays", BACKUP_DAYS);
    if (days <= 0 || days > 36500) return;
    const int64_t oldest = sh_now_ms() - days * 86400 * 1000;
    for (size_t k = 0; k < sizeof LOOSE / sizeof LOOSE[0]; ++k) {
        char *dir = sh_path_join(st->backup, LOOSE[k]);
        sh_strv names = SH_STRV_INIT;
        sh_list_dir(dir, SH_LIST_DIRS, &names);
        for (size_t i = 0; i < names.len; ++i) {
            const int64_t when = stamp_ms(names.items[i]);
            if (when < 0 || when >= oldest) continue;
            char *path = sh_path_join(dir, names.items[i]);
            remove_backup(path);
            sh_store_log(st, "removed an old backup: %s", names.items[i]);
            free(path);
        }
        sh_strv_free(&names);
        free(dir);
    }
}

// ---- the cache limit ----

typedef struct {
    const char *path;
    int64_t used, bytes;
} clone_row;

static int oldest_first(const void *a, const void *b) {
    const int64_t x = ((const clone_row *)a)->used, y = ((const clone_row *)b)->used;
    return x < y ? -1 : x > y;
}

// when it was last read: sh_repo_open leaves a mark; a clone that has none has not been used since this came in
static int64_t last_used(const char *clone) {
    char *mark = sh_path_join(clone, ".git/skillhub-used");
    const int64_t when = sh_mtime_ms(mark);
    free(mark);
    return when;
}

static void limit_cache(sh_store *st) {
    const long long max_mb = sh_setting_int("storage", "cacheMaxMB", CACHE_MAX_MB);
    if (max_mb <= 0 || max_mb > 1024 * 1024 * 1024) return;
    const int64_t limit = max_mb * 1024 * 1024;
    sh_strv clones = SH_STRV_INIT;
    sh_cache_clones(st, &clones);
    clone_row *rows = sh_xcalloc(clones.len + 1, sizeof *rows);
    int64_t total = 0;
    for (size_t i = 0; i < clones.len; ++i) {
        rows[i] = (clone_row){clones.items[i], last_used(clones.items[i]), bytes_of(clones.items[i])};
        total += rows[i].bytes;
    }
    if (total > limit && clones.len > 0) qsort(rows, clones.len, sizeof *rows, oldest_first);
    for (size_t i = 0; total > limit && i < clones.len; ++i) {
        if (!remove_clone(rows[i].path)) continue;
        total -= rows[i].bytes;
        sh_store_log(st, "the cache is over its limit: removed the clone %s, it comes back when needed", sh_path_name(rows[i].path));
    }
    free(rows);
    sh_strv_free(&clones);
}

void sh_storage_tidy(sh_store *st) {
    expire_loose(st);
    limit_cache(st);
}
