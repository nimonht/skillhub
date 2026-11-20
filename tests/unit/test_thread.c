// The scheduler of the engine's tasks (sh_parallel_sched): what runs, how many of one host at once, and in which order.
#include <stdbool.h>

#include "sh_thread.h"
#include "sh_time.h"
#include "test.h"

#define MAX_TASKS 256

typedef struct {
    const sh_task *tasks;
    long running[4];     // per host index (0 = a, 1 = b, 2 = none), now
    long peak[4];        // the most at once
    long runs[MAX_TASKS];  // times each task ran
    long seq;            // starts so far
    long started_at[MAX_TASKS];  // the start number of each task
    int sleep_ms;
    sh_mutex *m;
    sh_cond *cv;
    bool b_started;
    int wait_for_b;      // task that waits (up to 3 s) for host b's first task to start; -1: none
} probe;

static int host_index(const char *host) {
    if (!host || !*host) return 2;
    return host[0] == 'a' ? 0 : 1;
}

static void probe_task(size_t i, void *arg) {
    probe *p = arg;
    const int h = host_index(p->tasks ? p->tasks[i].host : NULL);
    sh_mutex_lock(p->m);
    p->started_at[i] = ++p->seq;
    ++p->runs[i];
    if (++p->running[h] > p->peak[h]) p->peak[h] = p->running[h];
    if (h == 1) {
        p->b_started = true;
        sh_cond_broadcast(p->cv);
    }
    if ((int)i == p->wait_for_b) {
        const int64_t until = sh_mono_ms() + 3000;
        while (!p->b_started && sh_mono_ms() < until) sh_cond_wait_ms(p->cv, p->m, 50);
    }
    sh_mutex_unlock(p->m);
    if (p->sleep_ms) sh_sleep_ms(p->sleep_ms);
    sh_mutex_lock(p->m);
    --p->running[h];
    sh_mutex_unlock(p->m);
}

static void probe_init(probe *p, const sh_task *tasks, int sleep_ms) {
    memset(p, 0, sizeof *p);
    p->tasks = tasks;
    p->sleep_ms = sleep_ms;
    p->m = sh_mutex_new();
    p->cv = sh_cond_new();
    p->wait_for_b = -1;
}

static void probe_free(probe *p) {
    sh_cond_free(p->cv);
    sh_mutex_free(p->m);
}

static void test_every_task_once(void) {
    sh_task tasks[200];
    for (int i = 0; i < 200; ++i) tasks[i] = (sh_task){i % 3 == 0 ? "a.example" : i % 3 == 1 ? "b.example" : "", i % 2 == 0, i % 7};
    probe p;
    probe_init(&p, tasks, 0);
    sh_parallel_sched(200, tasks, 8, (sh_host_limits){3, 3}, SH_SCHED_BATCH, probe_task, &p, NULL, 50);
    for (int i = 0; i < 200; ++i) CHECK(p.runs[i] == 1);
    CHECK(p.seq == 200);
    probe_free(&p);
}

static void test_per_host_cap(void) {
    sh_task tasks[40];
    for (int i = 0; i < 40; ++i) tasks[i] = (sh_task){i < 30 ? "a.example" : "b.example", true, 0};
    probe p;
    probe_init(&p, tasks, 5);
    sh_parallel_sched(40, tasks, 16, (sh_host_limits){3, 3}, SH_SCHED_BATCH, probe_task, &p, NULL, 50);
    CHECK(p.peak[0] <= 3);
    CHECK(p.peak[1] <= 3);
    CHECK(p.peak[0] >= 2);  // the cap is a limit, not the only speed: more than one ran at once
    probe_free(&p);
}

// A full host must not keep a worker from another host's task. Host a takes one at a time; its first task waits until
// host b's task, listed last, has started. A worker that sat on host a's queue would leave b's task waiting behind it.
// Heavy and light slots are counted apart: clones of a host leave room for checks of the same host, and the other way round.
static void test_heavy_and_light_apart(void) {
    sh_task tasks[24];
    for (int i = 0; i < 24; ++i) tasks[i] = (sh_task){"a.example", i < 12, 0};
    probe p;
    probe_init(&p, tasks, 10);
    sh_parallel_sched(24, tasks, 16, (sh_host_limits){2, 6}, SH_SCHED_BATCH, probe_task, &p, NULL, 50);
    CHECK(p.peak[0] <= 8);  // 2 heavy + 6 light
    CHECK(p.peak[0] >= 5);  // more than either limit alone would allow
    for (int i = 0; i < 24; ++i) CHECK(p.runs[i] == 1);
    probe_free(&p);
}

static void test_no_head_of_line_blocking(void) {
    sh_task tasks[6];
    for (int i = 0; i < 5; ++i) tasks[i] = (sh_task){"a.example", true, 0};
    tasks[5] = (sh_task){"b.example", true, 0};
    probe p;
    probe_init(&p, tasks, 0);
    p.wait_for_b = 0;
    const int64_t t0 = sh_mono_ms();
    sh_parallel_sched(6, tasks, 2, (sh_host_limits){1, 1}, SH_SCHED_BATCH, probe_task, &p, NULL, 50);
    CHECK(p.b_started);
    CHECK(p.started_at[5] == 2);         // the second task to start, right beside host a's first
    CHECK(sh_mono_ms() - t0 < 2500);     // and not after the wait ran out
    probe_free(&p);
}

static void test_costliest_first(void) {
    sh_task tasks[6] = {{"a.example", true, 1}, {"a.example", false, 5}, {"", false, 0}, {"b.example", true, 9}, {"a.example", true, 5}, {"b.example", false, 2}};
    probe p;
    probe_init(&p, tasks, 0);
    sh_parallel_sched(6, tasks, 1, (sh_host_limits){4, 4}, SH_SCHED_BATCH, probe_task, &p, NULL, 50);
    CHECK(p.started_at[3] == 1);  // cost 9
    CHECK(p.started_at[1] == 2);  // cost 5, listed before the other 5
    CHECK(p.started_at[4] == 3);
    CHECK(p.started_at[5] == 4);  // cost 2
    CHECK(p.started_at[0] == 5);
    CHECK(p.started_at[2] == 6);  // a local folder, cost 0
    probe_free(&p);
}

// Between equal costs the less busy host goes first, so two hosts take turns instead of one going first in full.
static void test_hosts_take_turns(void) {
    sh_task tasks[8];
    for (int i = 0; i < 8; ++i) tasks[i] = (sh_task){i < 4 ? "a.example" : "b.example", true, 0};
    probe p;
    probe_init(&p, tasks, 30);
    sh_parallel_sched(8, tasks, 2, (sh_host_limits){4, 4}, SH_SCHED_BATCH, probe_task, &p, NULL, 50);
    const long first_b = p.started_at[4];
    CHECK(first_b <= 2);  // b starts beside a's first task, not after all of a
    probe_free(&p);
}

static void test_local_fills_idle_workers(void) {
    sh_task tasks[6];
    for (int i = 0; i < 3; ++i) tasks[i] = (sh_task){"a.example", true, 10};
    for (int i = 3; i < 6; ++i) tasks[i] = (sh_task){"", false, 0};
    probe p;
    probe_init(&p, tasks, 20);
    sh_parallel_sched(6, tasks, 3, (sh_host_limits){1, 1}, SH_SCHED_BATCH, probe_task, &p, NULL, 50);
    CHECK(p.peak[0] == 1);
    CHECK(p.peak[2] >= 2);  // local tasks ran while host a was full
    probe_free(&p);
}

typedef struct {
    probe *p;
    sh_task *tasks;
    size_t n;
} batch_arg;

static void run_batch(void *arg) {
    batch_arg *b = arg;
    sh_parallel_sched(b->n, b->tasks, 8, (sh_host_limits){2, 2}, SH_SCHED_BATCH, probe_task, b->p, NULL, 50);
}

typedef struct {
    sh_mutex *m;
    long seq_at_start;
    long *seq;
} interactive_probe;

static void interactive_task(size_t i, void *arg) {
    (void)i;
    interactive_probe *q = arg;
    sh_mutex_lock(q->m);
    q->seq_at_start = *q->seq;
    sh_mutex_unlock(q->m);
}

// A run a person waits for gets the host's next free slot, ahead of the batch run's remaining tasks.
static void test_interactive_goes_first(void) {
    static sh_task tasks[40];
    for (int i = 0; i < 40; ++i) tasks[i] = (sh_task){"a.example", true, 0};
    probe p;
    probe_init(&p, tasks, 15);
    batch_arg b = {&p, tasks, 40};
    sh_thread *t = sh_thread_start(run_batch, &b);
    CHECK(t != NULL);
    long seen = 0;
    for (int tries = 0; tries < 400 && seen < 4; ++tries) {  // wait until the batch run is well under way
        sh_sleep_ms(5);
        sh_mutex_lock(p.m);
        seen = p.seq;
        sh_mutex_unlock(p.m);
    }
    interactive_probe q = {p.m, -1, &p.seq};
    const sh_task mine = {"a.example", true, 0};
    sh_parallel_sched(1, &mine, 1, (sh_host_limits){2, 2}, SH_SCHED_INTERACTIVE, interactive_task, &q, NULL, 50);
    sh_thread_join(t);
    CHECK(q.seq_at_start >= 0);
    // two batch tasks hold host a's slots when it arrives, and perhaps two more started before it registered; without the
    // priority it would wait for the batch run's turn among 40
    CHECK(q.seq_at_start <= seen + 4);
    CHECK(p.peak[0] <= 2);
    probe_free(&p);
}

static void test_parallel_for_in_order(void) {
    probe p;
    probe_init(&p, NULL, 0);
    sh_parallel_for(50, 1, probe_task, &p, NULL, 50);
    for (int i = 0; i < 50; ++i) CHECK(p.started_at[i] == i + 1);
    probe_free(&p);
}

const sh_test_case sh_tests_thread[] = {
    {"thread.every_task_once", test_every_task_once},
    {"thread.per_host_cap", test_per_host_cap},
    {"thread.heavy_and_light_apart", test_heavy_and_light_apart},
    {"thread.no_head_of_line_blocking", test_no_head_of_line_blocking},
    {"thread.costliest_first", test_costliest_first},
    {"thread.hosts_take_turns", test_hosts_take_turns},
    {"thread.local_fills_idle_workers", test_local_fills_idle_workers},
    {"thread.interactive_goes_first", test_interactive_goes_first},
    {"thread.parallel_for_in_order", test_parallel_for_in_order},
    {NULL, NULL},
};
