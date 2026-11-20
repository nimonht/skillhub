// Threads for the engine: a mutex, a condition variable, and the scheduler that runs a job's tasks on worker threads.
// Each repository of a job is one task; git itself runs as a separate process per command.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sh_mutex sh_mutex;
typedef struct sh_cond sh_cond;

sh_mutex *sh_mutex_new(void);
void sh_mutex_free(sh_mutex *m);
void sh_mutex_lock(sh_mutex *m);
void sh_mutex_unlock(sh_mutex *m);

sh_cond *sh_cond_new(void);
void sh_cond_free(sh_cond *c);
void sh_cond_wait(sh_cond *c, sh_mutex *m);
bool sh_cond_wait_ms(sh_cond *c, sh_mutex *m, int ms);  // false on timeout
void sh_cond_broadcast(sh_cond *c);

// What the scheduler knows of a task before it runs.
typedef struct {
    const char *host;  // the server the task talks to, lower case, for the whole of its run; NULL or "": none (a local folder)
    bool heavy;        // it downloads a lot (a new clone), and counts against the host's heavy limit instead of its light one
    long cost;         // what the task is expected to take, in any unit: the costliest one that can start goes first
} sh_task;

// How many tasks of one host run at once, counted over every run in the process.
typedef struct {
    int heavy;  // downloads that can take tens of MB each
    int light;  // the rest: mostly network round trips
} sh_host_limits;

// Who waits for a run: a person, for one or two skills (read or checked while a long job goes on), or nobody in particular.
typedef enum { SH_SCHED_BATCH, SH_SCHED_INTERACTIVE } sh_sched_class;

// Runs fn(i, ctx) for every i in [0, n) on up to `workers` threads and returns when all are done. Meanwhile the calling
// thread runs idle(ctx) at least every `idle_ms` milliseconds (the engine commits finished work to the lock file there).
//
// A task with a host holds one of that host's slots while it runs, heavy or light: at most `limits` of each at once. A free
// worker takes the costliest task that can start now, from the least busy host when costs are equal, and waits only when
// nothing can start: a host that is full never keeps a worker from another host's task or from a local one. An
// interactive run gets a host's next free slot before a batch run does. `tasks` NULL: no hosts, in order.
void sh_parallel_sched(size_t n, const sh_task *tasks, int workers, sh_host_limits limits, sh_sched_class cls,
                       void (*fn)(size_t i, void *ctx), void *ctx, void (*idle)(void *ctx), int idle_ms);

// sh_parallel_sched without hosts or costs: the tasks start in order.
void sh_parallel_for(size_t n, int workers, void (*fn)(size_t i, void *ctx), void *ctx, void (*idle)(void *ctx), int idle_ms);

// A thread of its own (a job of `skillhub serve`); sh_thread_join waits for it and frees it.
typedef struct sh_thread sh_thread;
sh_thread *sh_thread_start(void (*fn)(void *arg), void *arg);  // NULL when the system refused
void sh_thread_join(sh_thread *t);

int sh_cpu_count(void);
long sh_atomic_inc(long *v);  // ++*v from any thread; returns the new value
long sh_atomic_add(long *v, long n);  // *v += n from any thread; returns the new value
long sh_atomic_get(long *v);  // *v, read so that a write from another thread is seen whole

#ifdef __cplusplus
}
#endif
