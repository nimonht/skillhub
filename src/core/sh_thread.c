#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE
#endif

#include "sh_thread.h"

#include <stdlib.h>
#include <string.h>

#include "sh_str.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#endif

#ifdef _WIN32

struct sh_mutex { SRWLOCK lock; };
struct sh_cond { CONDITION_VARIABLE cv; };

sh_mutex *sh_mutex_new(void) {
    sh_mutex *m = sh_xcalloc(1, sizeof *m);
    InitializeSRWLock(&m->lock);
    return m;
}
void sh_mutex_free(sh_mutex *m) { free(m); }
void sh_mutex_lock(sh_mutex *m) { AcquireSRWLockExclusive(&m->lock); }
void sh_mutex_unlock(sh_mutex *m) { ReleaseSRWLockExclusive(&m->lock); }

sh_cond *sh_cond_new(void) {
    sh_cond *c = sh_xcalloc(1, sizeof *c);
    InitializeConditionVariable(&c->cv);
    return c;
}
void sh_cond_free(sh_cond *c) { free(c); }
void sh_cond_wait(sh_cond *c, sh_mutex *m) { SleepConditionVariableSRW(&c->cv, &m->lock, INFINITE, 0); }
bool sh_cond_wait_ms(sh_cond *c, sh_mutex *m, int ms) { return SleepConditionVariableSRW(&c->cv, &m->lock, (DWORD)ms, 0) != 0; }
void sh_cond_broadcast(sh_cond *c) { WakeAllConditionVariable(&c->cv); }

int sh_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}

long sh_atomic_inc(long *v) { return InterlockedIncrement((volatile LONG *)v); }  // long and LONG are both 32 bits here
long sh_atomic_add(long *v, long n) { return InterlockedExchangeAdd((volatile LONG *)v, n) + n; }
long sh_atomic_get(long *v) { return InterlockedCompareExchange((volatile LONG *)v, 0, 0); }

#else

struct sh_mutex { pthread_mutex_t lock; };
struct sh_cond { pthread_cond_t cv; };

sh_mutex *sh_mutex_new(void) {
    sh_mutex *m = sh_xcalloc(1, sizeof *m);
    pthread_mutex_init(&m->lock, NULL);
    return m;
}
void sh_mutex_free(sh_mutex *m) {
    if (!m) return;
    pthread_mutex_destroy(&m->lock);
    free(m);
}
void sh_mutex_lock(sh_mutex *m) { pthread_mutex_lock(&m->lock); }
void sh_mutex_unlock(sh_mutex *m) { pthread_mutex_unlock(&m->lock); }

sh_cond *sh_cond_new(void) {
    sh_cond *c = sh_xcalloc(1, sizeof *c);
    pthread_cond_init(&c->cv, NULL);
    return c;
}
void sh_cond_free(sh_cond *c) {
    if (!c) return;
    pthread_cond_destroy(&c->cv);
    free(c);
}
void sh_cond_wait(sh_cond *c, sh_mutex *m) { pthread_cond_wait(&c->cv, &m->lock); }

bool sh_cond_wait_ms(sh_cond *c, sh_mutex *m, int ms) {
    struct timeval now;
    gettimeofday(&now, NULL);
    struct timespec until;
    long long ns = (long long)now.tv_usec * 1000 + (long long)(ms % 1000) * 1000000;
    until.tv_sec = now.tv_sec + ms / 1000 + (time_t)(ns / 1000000000);
    until.tv_nsec = (long)(ns % 1000000000);
    return pthread_cond_timedwait(&c->cv, &m->lock, &until) != ETIMEDOUT;
}

void sh_cond_broadcast(sh_cond *c) { pthread_cond_broadcast(&c->cv); }

int sh_cpu_count(void) {
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

long sh_atomic_inc(long *v) { return __atomic_add_fetch(v, 1, __ATOMIC_SEQ_CST); }
long sh_atomic_add(long *v, long n) { return __atomic_add_fetch(v, n, __ATOMIC_SEQ_CST); }
long sh_atomic_get(long *v) { return __atomic_load_n(v, __ATOMIC_SEQ_CST); }

#endif

// ---- parallel loop ----

typedef struct {
    size_t n, next, finished;
    void (*fn)(size_t, void *);
    void *ctx;
    sh_mutex *m;
    sh_cond *done;
} loop_state;

#ifdef _WIN32
static DWORD WINAPI worker(void *arg) {
#else
static void *worker(void *arg) {
#endif
    loop_state *s = arg;
    for (;;) {
        sh_mutex_lock(s->m);
        const size_t i = s->next < s->n ? s->next++ : s->n;
        sh_mutex_unlock(s->m);
        if (i >= s->n) break;
        s->fn(i, s->ctx);
        sh_mutex_lock(s->m);
        ++s->finished;
        sh_cond_broadcast(s->done);
        sh_mutex_unlock(s->m);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

void sh_parallel_for(size_t n, int workers, void (*fn)(size_t, void *), void *ctx, void (*idle)(void *), int idle_ms) {
    if (n == 0) return;
    if (workers < 1) workers = 1;
    if ((size_t)workers > n) workers = (int)n;
    loop_state s = {n, 0, 0, fn, ctx, sh_mutex_new(), sh_cond_new()};
#ifdef _WIN32
    HANDLE *threads = sh_xcalloc((size_t)workers, sizeof *threads);
    for (int t = 0; t < workers; ++t) threads[t] = CreateThread(NULL, 0, worker, &s, 0, NULL);
#else
    pthread_t *threads = sh_xcalloc((size_t)workers, sizeof *threads);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);  // the tasks use little stack; keeps many workers cheap
    for (int t = 0; t < workers; ++t) pthread_create(&threads[t], &attr, worker, &s);
    pthread_attr_destroy(&attr);
#endif
    sh_mutex_lock(s.m);
    while (s.finished < s.n) {
        sh_cond_wait_ms(s.done, s.m, idle_ms > 0 ? idle_ms : 1000);
        if (idle) {
            sh_mutex_unlock(s.m);
            idle(ctx);
            sh_mutex_lock(s.m);
        }
    }
    sh_mutex_unlock(s.m);
    for (int t = 0; t < workers; ++t) {
#ifdef _WIN32
        WaitForSingleObject(threads[t], INFINITE);
        CloseHandle(threads[t]);
#else
        pthread_join(threads[t], NULL);
#endif
    }
    free(threads);
    sh_cond_free(s.done);
    sh_mutex_free(s.m);
}

// ---- threads of their own ----

struct sh_thread {
    void (*fn)(void *);
    void *arg;
#ifdef _WIN32
    HANDLE h;
#else
    pthread_t t;
#endif
};

#ifdef _WIN32
static DWORD WINAPI thread_main(void *p) {
    sh_thread *t = p;
    t->fn(t->arg);
    return 0;
}
#else
static void *thread_main(void *p) {
    sh_thread *t = p;
    t->fn(t->arg);
    return NULL;
}
#endif

sh_thread *sh_thread_start(void (*fn)(void *arg), void *arg) {
    sh_thread *t = sh_xcalloc(1, sizeof *t);
    t->fn = fn;
    t->arg = arg;
#ifdef _WIN32
    t->h = CreateThread(NULL, 0, thread_main, t, 0, NULL);
    const bool ok = t->h != NULL;
#else
    const bool ok = pthread_create(&t->t, NULL, thread_main, t) == 0;
#endif
    if (!ok) {
        free(t);
        return NULL;
    }
    return t;
}

void sh_thread_join(sh_thread *t) {
    if (!t) return;
#ifdef _WIN32
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
#else
    pthread_join(t->t, NULL);
#endif
    free(t);
}

// ---- per-host limits ----

typedef struct {
    char *host;
    int used;
} host_slot;

struct sh_limiter {
    int per_host;
    host_slot *slots;
    size_t n, cap;
    sh_mutex *m;
    sh_cond *freed;
};

sh_limiter *sh_limiter_new(int per_host) {
    sh_limiter *l = sh_xcalloc(1, sizeof *l);
    l->per_host = per_host > 0 ? per_host : 1;
    l->m = sh_mutex_new();
    l->freed = sh_cond_new();
    return l;
}

void sh_limiter_free(sh_limiter *l) {
    if (!l) return;
    for (size_t i = 0; i < l->n; ++i) free(l->slots[i].host);
    free(l->slots);
    sh_cond_free(l->freed);
    sh_mutex_free(l->m);
    free(l);
}

static host_slot *slot_for(sh_limiter *l, const char *host) {
    for (size_t i = 0; i < l->n; ++i)
        if (strcmp(l->slots[i].host, host) == 0) return &l->slots[i];
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->slots = sh_xrealloc(l->slots, l->cap * sizeof *l->slots);
    }
    l->slots[l->n].host = sh_strdup(host);
    l->slots[l->n].used = 0;
    return &l->slots[l->n++];
}

void sh_limiter_acquire(sh_limiter *l, const char *host) {
    sh_mutex_lock(l->m);
    while (slot_for(l, host)->used >= l->per_host) sh_cond_wait(l->freed, l->m);
    slot_for(l, host)->used++;
    sh_mutex_unlock(l->m);
}

void sh_limiter_release(sh_limiter *l, const char *host) {
    sh_mutex_lock(l->m);
    host_slot *s = slot_for(l, host);
    if (s->used > 0) s->used--;
    sh_cond_broadcast(l->freed);
    sh_mutex_unlock(l->m);
}
