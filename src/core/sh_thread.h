// Threads for the engine: a mutex, a condition variable, a parallel loop, and per-host limits for network git commands.
// Each repository of a job is one task on a worker thread; git itself runs as a separate process per command.
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

// Runs fn(i, ctx) for every i in [0, n) on up to `workers` threads and returns when all are done. Meanwhile the calling
// thread runs idle(ctx) at least every `idle_ms` milliseconds (the engine commits finished work to the lock file there).
void sh_parallel_for(size_t n, int workers, void (*fn)(size_t i, void *ctx), void *ctx, void (*idle)(void *ctx), int idle_ms);

// At most `per_host` holders per host name at a time.
typedef struct sh_limiter sh_limiter;
sh_limiter *sh_limiter_new(int per_host);
void sh_limiter_free(sh_limiter *l);
void sh_limiter_acquire(sh_limiter *l, const char *host);
void sh_limiter_release(sh_limiter *l, const char *host);

int sh_cpu_count(void);
long sh_atomic_inc(long *v);  // ++*v from any thread; returns the new value

#ifdef __cplusplus
}
#endif
