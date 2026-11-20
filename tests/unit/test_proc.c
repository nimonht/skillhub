// Processes: the background class of automatic runs (sh_proc_background).
#if defined(__linux__)
#define _GNU_SOURCE
#endif

#include <stdbool.h>

#include "sh_proc.h"
#include "test.h"

#if defined(__linux__)
#include <errno.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

// policy and nice of a program it starts, from that program's own /proc/self/stat
static bool child_policy(int *policy, int *nice) {
    const char *argv[] = {"cat", "/proc/self/stat", NULL};
    sh_proc_opts o;
    memset(&o, 0, sizeof o);
    o.timeout_ms = 10000;
    sh_proc_result r;
    const bool ok = sh_proc_run(argv, &o, &r);
    const char *paren = ok ? strrchr(sh_buf_str(&r.out), ')') : NULL;  // the name before it may hold spaces
    bool found = false;
    if (paren) {
        // after ") ": field 3 is the state; nice is field 19 and policy field 41
        int field = 3;
        long nice_v = 0, policy_v = -1;
        for (const char *p = paren + 2; *p; ++field) {
            char *end;
            const long v = strtol(p, &end, 10);
            if (field == 19) nice_v = v;
            if (field == 41) policy_v = v;
            const char *space = strchr(p, ' ');
            if (!space) break;
            p = space + 1;
        }
        *nice = (int)nice_v;
        *policy = (int)policy_v;
        found = policy_v >= 0;
    }
    sh_proc_result_free(&r);
    return found;
}

// what a forked child finds after sh_proc_background, as bits: each one that is missing
static int background_check(void) {
    int bad = 0;
    sh_proc_background();
    if (!sh_proc_in_background()) bad |= 1;
    if (sched_getscheduler(0) != SCHED_BATCH) bad |= 2;
    errno = 0;
    if (getpriority(PRIO_PROCESS, 0) < 10) bad |= 4;
    const long io = syscall(SYS_ioprio_get, 1, 0);
    if (io != ((2 << 13) | 7)) bad |= 8;
    if (prctl(PR_GET_TIMERSLACK) != 50L * 1000 * 1000) bad |= 16;
    int policy = -1, nice = 0;
    if (!child_policy(&policy, &nice) || policy != SCHED_BATCH || nice < 10) bad |= 32;  // git inherits it
    return bad;
}

static void test_background(void) {
    fflush(NULL);
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) _exit(background_check());
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    CHECK(WIFEXITED(status));
    const int bad = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (bad) fprintf(stderr, "background: missing bits %d (1 flag, 2 policy, 4 nice, 8 I/O, 16 slack, 32 child)\n", bad);
    CHECK(bad == 0);
    CHECK(!sh_proc_in_background());  // only the child moved
}

// a nice already above 10 is kept, not raised back
static void test_background_keeps_lower_priority(void) {
    fflush(NULL);
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        setpriority(PRIO_PROCESS, 0, 15);
        sh_proc_background();
        errno = 0;
        _exit(getpriority(PRIO_PROCESS, 0) == 15 ? 0 : 1);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
#else
static void test_background(void) { CHECK(!sh_proc_in_background()); }
static void test_background_keeps_lower_priority(void) {}
#endif

const sh_test_case sh_tests_proc[] = {
    {"proc.background", test_background},
    {"proc.background_keeps_lower_priority", test_background_keeps_lower_priority},
    {NULL, NULL},
};
