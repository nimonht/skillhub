// Closing a terminal ends what runs in it (POSIX). An interactive shell gives each command a process group of its own, and some
// programs (codex) catch the hangup and run on: closing the terminal must still end them. A program that left the session on
// purpose (setsid) is not the terminal's any more, and stays. This does what the window does when a tab closes (VTermItem's
// destructor): hang up, wait, kill, free.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sh_pty.h"

static void write_pid(const char *file) {
    FILE *f = fopen(file, "w");
    if (!f) _exit(1);
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);
}

// the programs the shell runs: this same file, started again
static int stubborn(const char *file) {  // a job of its own that ignores the hangup and keeps the terminal
    setpgid(0, 0);
    signal(SIGHUP, SIG_IGN);
    write_pid(file);
    for (int i = 0; i < 600; ++i) usleep(100000);  // never outlives a failed run by long
    return 0;
}

static int detached(const char *file) {  // left the session: not the terminal's to end
    setsid();
    write_pid(file);
    for (int i = 0; i < 600; ++i) usleep(100000);
    return 0;
}

static sh_pty *pty;
static volatile int reader_done;

static void *reader(void *unused) {
    (void)unused;
    char buf[4096];
    while (sh_pty_read(pty, buf, sizeof buf) > 0) {}
    reader_done = 1;
    return NULL;
}

static int wait_for(volatile int *flag, int ms) {
    for (int i = 0; i < ms / 10 && !*flag; ++i) usleep(10000);
    return *flag;
}

static pid_t read_pid(const char *file, int ms) {
    for (int i = 0; i < ms / 10; ++i) {
        FILE *f = fopen(file, "r");
        int pid = 0;
        if (f) {
            const int got = fscanf(f, "%d", &pid);
            fclose(f);
            if (got == 1 && pid > 1) return (pid_t)pid;
        }
        usleep(10000);
    }
    return 0;
}

static int zombie(pid_t pid) {  // ended, but nobody reaped it: a container's PID 1 often does not
#ifdef __linux__
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *close = strrchr(buf, ')');
    return close && close[1] == ' ' && close[2] == 'Z';
#else
    (void)pid;
    return 0;
#endif
}

static int gone(pid_t pid, int ms) {
    for (int i = 0; i < ms / 10; ++i) {
        if ((kill(pid, 0) != 0 && errno == ESRCH) || zombie(pid)) return 1;
        usleep(10000);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--stubborn") == 0) return stubborn(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--detached") == 0) return detached(argv[2]);

    char dir[] = "/tmp/skillhub-pty-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    char held[256], left[256], script[2048];
    snprintf(held, sizeof held, "%s/stubborn", dir);
    snprintf(left, sizeof left, "%s/detached", dir);
    snprintf(script, sizeof script, "'%s' --stubborn '%s' & '%s' --detached '%s' </dev/null >/dev/null 2>&1 & wait", argv[0], held, argv[0], left);
    const char *args[] = {"sh", "-c", script, NULL};
    const char *env[] = {"TERM=xterm-256color", NULL};
    char *err = NULL;
    pty = sh_pty_spawn("/bin/sh", args, dir, env, 80, 24, &err);
    if (!pty) {
        fprintf(stderr, "cannot start the shell: %s\n", err ? err : "?");
        return 1;
    }
    pthread_t thread;
    pthread_create(&thread, NULL, reader, NULL);
    const pid_t stubborn_pid = read_pid(held, 5000), detached_pid = read_pid(left, 5000);
    int failures = 0;
    if (!stubborn_pid || !detached_pid) {
        fprintf(stderr, "the programs did not start\n");
        ++failures;
    }

    sh_pty_hangup(pty);
    if (!wait_for(&reader_done, 1000)) {
        sh_pty_kill(pty);
        if (!wait_for(&reader_done, 2000)) {
            fprintf(stderr, "the terminal is still held after the kill: a program in it was missed\n");
            ++failures;
            if (stubborn_pid) kill(stubborn_pid, SIGKILL);
            wait_for(&reader_done, 2000);
        }
    }
    pthread_join(thread, NULL);
    sh_pty_free(pty);

    if (stubborn_pid && !gone(stubborn_pid, 3000)) {
        fprintf(stderr, "a program that ignores the hangup outlived its terminal\n");
        kill(stubborn_pid, SIGKILL);
        ++failures;
    }
    if (detached_pid) {
        if (kill(detached_pid, 0) != 0) {
            fprintf(stderr, "a program that left the session was ended with the terminal\n");
            ++failures;
        }
        kill(detached_pid, SIGKILL);
    }
    unlink(held);
    unlink(left);
    rmdir(dir);
    if (!failures) printf("pty: closing a terminal ends its programs and spares a detached one\n");
    return failures ? 1 : 0;
}
