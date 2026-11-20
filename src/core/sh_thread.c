#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE
#endif

#include "sh_thread.h"

#include <stdint.h>
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

// ---- the scheduler ----
//
// Borrowed from the CPU schedulers of sched_ext and from the CAKE queue discipline, at the scale of a job's repositories:
// tasks wait in queues by host, a host's share is capped (CAKE's isolation of hosts), the costliest task that can start
// goes first so that a long one does not run alone at the end (longest processing time first), and the work a person
// waits for goes before work nobody waits for (LAVD's latency-critical tasks). Everything below is guarded by one lock
// for the whole process, since the slots of a host are shared by every run.

#ifdef _WIN32
static sh_mutex sched_m = {SRWLOCK_INIT};
static sh_cond sched_cv = {CONDITION_VARIABLE_INIT};  // a task ended or a slot came free, in any run
#else
static sh_mutex sched_m = {PTHREAD_MUTEX_INITIALIZER};
static sh_cond sched_cv = {PTHREAD_COND_INITIALIZER};
#endif

typedef struct host_slots {
    char *host;
    int used[2];    // tasks of any run that hold one of its light [0] or heavy [1] slots now
    int wanted[2];  // tasks of interactive runs that have not started: a batch run leaves them as many slots
    int runs;       // runs with tasks for this host; the entry goes when none is left
    struct host_slots *next;
} host_slots;

static host_slots *all_hosts;

static host_slots *host_slots_for(const char *host) {
    for (host_slots *h = all_hosts; h; h = h->next)
        if (strcmp(h->host, host) == 0) return h;
    host_slots *h = sh_xcalloc(1, sizeof *h);
    h->host = sh_strdup(host);
    h->next = all_hosts;
    all_hosts = h;
    return h;
}

static void host_slots_drop(host_slots *gone) {
    for (host_slots **p = &all_hosts; *p; p = &(*p)->next) {
        if (*p == gone) {
            *p = gone->next;
            free(gone->host);
            free(gone);
            return;
        }
    }
}

typedef struct {
    host_slots *slots;  // NULL: tasks that need no host
    int heavy;          // 1: the host's heavy slots, 0: its light ones
    size_t *order;      // its tasks, costliest first
    size_t n, next;     // order[next] starts next
    int wanted;         // what this run added to slots->wanted[heavy] and has not taken back
} task_queue;

typedef struct {
    task_queue *queues;
    size_t n_queues;
    size_t n, left, finished;  // tasks; not started yet; done
    const sh_task *tasks;
    int limit[2];  // light, heavy
    bool interactive;
    void (*fn)(size_t, void *);
    void *ctx;
} sched_run;

static long cost_of(const sched_run *r, size_t i) { return r->tasks ? r->tasks[i].cost : 0; }

// `a` before `b`: costlier first; then the host with fewer tasks running (a queue without a host counts none); then the
// task that came first
static bool goes_before(const sched_run *r, const task_queue *a, const task_queue *b) {
    const size_t ia = a->order[a->next], ib = b->order[b->next];
    const long ca = cost_of(r, ia), cb = cost_of(r, ib);
    if (ca != cb) return ca > cb;
    const int ua = a->slots ? a->slots->used[0] + a->slots->used[1] : 0, ub = b->slots ? b->slots->used[0] + b->slots->used[1] : 0;
    if (ua != ub) return ua < ub;
    return ia < ib;
}

// the queue whose next task starts now, or NULL when none can (sched_m held)
static task_queue *pick(sched_run *r) {
    task_queue *best = NULL;
    for (size_t k = 0; k < r->n_queues; ++k) {
        task_queue *q = &r->queues[k];
        if (q->next == q->n) continue;
        if (q->slots) {
            const int taken = q->slots->used[q->heavy] + (r->interactive ? 0 : q->slots->wanted[q->heavy]);
            if (taken >= r->limit[q->heavy]) continue;
        }
        if (!best || goes_before(r, q, best)) best = q;
    }
    return best;
}

static void sched_work(sched_run *r) {
    sh_mutex_lock(&sched_m);
    while (r->left > 0) {
        task_queue *q = pick(r);
        if (!q) {  // every host this run needs is full: wait for a slot, never sit on one
            sh_cond_wait(&sched_cv, &sched_m);
            continue;
        }
        const size_t i = q->order[q->next++];
        --r->left;
        if (q->slots) {
            ++q->slots->used[q->heavy];
            if (q->wanted > 0) {
                --q->wanted;
                --q->slots->wanted[q->heavy];
            }
        }
        sh_mutex_unlock(&sched_m);
        r->fn(i, r->ctx);
        sh_mutex_lock(&sched_m);
        if (q->slots) --q->slots->used[q->heavy];
        ++r->finished;
        sh_cond_broadcast(&sched_cv);
    }
    sh_mutex_unlock(&sched_m);
}

#ifdef _WIN32
static DWORD WINAPI worker(void *arg) {
    sched_work(arg);
    return 0;
}
#else
static void *worker(void *arg) {
    sched_work(arg);
    return NULL;
}
#endif

typedef struct {
    size_t queue, i;
    long cost;
} sort_key;

static int cmp_key(const void *a, const void *b) {
    const sort_key *x = a, *y = b;
    if (x->queue != y->queue) return x->queue < y->queue ? -1 : 1;
    if (x->cost != y->cost) return x->cost > y->cost ? -1 : 1;
    return x->i < y->i ? -1 : x->i > y->i;
}

// one queue for each host and kind of slot, plus one for the tasks without a host; registers the run's hosts (sched_m held)
static void build_queues(sched_run *r) {
    task_queue *qs = sh_xcalloc(r->n + 1, sizeof *qs);
    size_t nq = 0, local = SIZE_MAX;
    sort_key *keys = sh_xcalloc(r->n, sizeof *keys);
    for (size_t i = 0; i < r->n; ++i) {
        const sh_task *t = r->tasks ? &r->tasks[i] : NULL;
        size_t k;
        if (!t || !t->host || !*t->host) {
            if (local == SIZE_MAX) local = nq++;
            k = local;
        } else {
            host_slots *h = host_slots_for(t->host);
            const int heavy = t->heavy ? 1 : 0;
            for (k = 0; k < nq; ++k)
                if (qs[k].slots == h && qs[k].heavy == heavy) break;
            if (k == nq) {
                qs[nq].slots = h;
                qs[nq].heavy = heavy;
                ++nq;
                bool counted = false;  // a host counts each run once, whichever kinds of slot it uses
                for (size_t o = 0; o + 1 < nq && !counted; ++o) counted = qs[o].slots == h;
                if (!counted) ++h->runs;
            }
        }
        keys[i] = (sort_key){k, i, cost_of(r, i)};
    }
    qsort(keys, r->n, sizeof *keys, cmp_key);
    for (size_t j = 0; j < r->n;) {
        task_queue *q = &qs[keys[j].queue];
        size_t end = j;
        while (end < r->n && keys[end].queue == keys[j].queue) ++end;
        q->n = end - j;
        q->order = sh_xcalloc(q->n, sizeof *q->order);
        for (size_t t = 0; t < q->n; ++t) q->order[t] = keys[j + t].i;
        if (r->interactive && q->slots) {
            q->wanted = (int)q->n;
            q->slots->wanted[q->heavy] += q->wanted;
        }
        j = end;
    }
    free(keys);
    r->queues = qs;
    r->n_queues = nq;
}

// takes back what the run registered (sched_m held)
static void drop_queues(sched_run *r) {
    for (size_t k = 0; k < r->n_queues; ++k) {
        task_queue *q = &r->queues[k];
        if (q->slots) q->slots->wanted[q->heavy] -= q->wanted;
    }
    for (size_t k = 0; k < r->n_queues; ++k) {
        task_queue *q = &r->queues[k];
        bool last = q->slots != NULL;  // the last queue of its host in this run lets go of the host
        for (size_t o = k + 1; o < r->n_queues && last; ++o) last = r->queues[o].slots != q->slots;
        if (last && --q->slots->runs == 0) host_slots_drop(q->slots);
        free(q->order);
    }
    free(r->queues);
}

void sh_parallel_sched(size_t n, const sh_task *tasks, int workers, sh_host_limits limits, sh_sched_class cls, void (*fn)(size_t, void *),
                       void *ctx, void (*idle)(void *), int idle_ms) {
    if (n == 0) return;
    if (workers < 1) workers = 1;
    if ((size_t)workers > n) workers = (int)n;
    sched_run r = {0};
    r.n = r.left = n;
    r.tasks = tasks;
    r.limit[0] = limits.light > 0 ? limits.light : 1;
    r.limit[1] = limits.heavy > 0 ? limits.heavy : 1;
    r.interactive = cls == SH_SCHED_INTERACTIVE;
    r.fn = fn;
    r.ctx = ctx;
    sh_mutex_lock(&sched_m);
    build_queues(&r);
    sh_mutex_unlock(&sched_m);
    int started = 0;
#ifdef _WIN32
    HANDLE *threads = sh_xcalloc((size_t)workers, sizeof *threads);
    for (int t = 0; t < workers; ++t) {
        HANDLE h = CreateThread(NULL, 0, worker, &r, 0, NULL);
        if (h) threads[started++] = h;
    }
#else
    pthread_t *threads = sh_xcalloc((size_t)workers, sizeof *threads);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);  // the tasks use little stack; keeps many workers cheap
    for (int t = 0; t < workers; ++t)
        if (pthread_create(&threads[started], &attr, worker, &r) == 0) ++started;
    pthread_attr_destroy(&attr);
#endif
    if (started == 0) sched_work(&r);  // no thread could start: the caller does the work
    sh_mutex_lock(&sched_m);
    while (r.finished < r.n) {
        sh_cond_wait_ms(&sched_cv, &sched_m, idle_ms > 0 ? idle_ms : 1000);
        if (idle) {
            sh_mutex_unlock(&sched_m);
            idle(ctx);
            sh_mutex_lock(&sched_m);
        }
    }
    sh_mutex_unlock(&sched_m);
    for (int t = 0; t < started; ++t) {
#ifdef _WIN32
        WaitForSingleObject(threads[t], INFINITE);
        CloseHandle(threads[t]);
#else
        pthread_join(threads[t], NULL);
#endif
    }
    free(threads);
    sh_mutex_lock(&sched_m);
    drop_queues(&r);
    sh_cond_broadcast(&sched_cv);  // what it held back for itself is free for the other runs
    sh_mutex_unlock(&sched_m);
}

void sh_parallel_for(size_t n, int workers, void (*fn)(size_t, void *), void *ctx, void (*idle)(void *), int idle_ms) {
    sh_parallel_sched(n, NULL, workers, (sh_host_limits){1, 1}, SH_SCHED_BATCH, fn, ctx, idle, idle_ms);
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
