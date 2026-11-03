#if !defined(__APPLE__)
#define _GNU_SOURCE  // ptsname_r
#endif

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include "sh_pty.h"

extern char **environ;

typedef struct {
    pid_t pid, pgid;
} member;

struct sh_pty {
    int fd;
    pid_t pid;
    member *members;  // the session as the hangup found it: macOS no longer finds a session once its leader has exited
    size_t n_members;
};

// out of memory ends the program, as sh_xmalloc does in the core, which is not linked here
static void *need(void *p) {
    if (!p) abort();
    return p;
}

static char *message(const char *what) {
    char *m = malloc(256);
    if (m) snprintf(m, 256, "%s: %s", what, strerror(errno));
    return m;
}

sh_pty *sh_pty_spawn(const char *program, const char *const *argv, const char *cwd, const char *const *env, int cols, int rows, char **err) {
    const int fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0 || grantpt(fd) != 0 || unlockpt(fd) != 0) {
        if (err) *err = message("cannot open a terminal");
        if (fd >= 0) close(fd);
        return NULL;
    }
    char name[256];
#ifdef __APPLE__
    const char *found = ptsname(fd);
    snprintf(name, sizeof name, "%s", found ? found : "");
#else
    if (ptsname_r(fd, name, sizeof name) != 0) name[0] = 0;
#endif
    const struct winsize size = {(unsigned short)rows, (unsigned short)cols, 0, 0};
    ioctl(fd, TIOCSWINSZ, &size);
    // everything the child needs is built here: between fork and exec only what is safe in a threaded process may run
    size_t n_old = 0, n_new = 0;
    while (environ[n_old]) ++n_old;
    while (env && env[n_new]) ++n_new;
    char **envp = need(calloc(n_old + n_new + 1, sizeof *envp));
    size_t k = 0;
    for (size_t i = 0; i < n_old; ++i) {
        const char *eq = strchr(environ[i], '=');
        bool replaced = false;
        for (size_t j = 0; eq && j < n_new && !replaced; ++j) replaced = strncmp(env[j], environ[i], (size_t)(eq - environ[i]) + 1) == 0;
        if (!replaced) envp[k++] = environ[i];
    }
    for (size_t j = 0; j < n_new; ++j) envp[k++] = (char *)env[j];
    const pid_t pid = fork();
    if (pid < 0) {
        if (err) *err = message("cannot start the shell");
        free(envp);
        close(fd);
        return NULL;
    }
    if (pid == 0) {
        setsid();
        const int slave = open(name, O_RDWR);
        if (slave < 0) _exit(127);
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) close(slave);
        if (cwd && *cwd && chdir(cwd) != 0) (void)!chdir("/");
        struct sigaction dfl;
        memset(&dfl, 0, sizeof dfl);
        dfl.sa_handler = SIG_DFL;
        sigaction(SIGPIPE, &dfl, NULL);
        sigaction(SIGINT, &dfl, NULL);
        sigaction(SIGQUIT, &dfl, NULL);
        execve(program, (char *const *)argv, envp);
        _exit(127);
    }
    free(envp);
    sh_pty *p = need(calloc(1, sizeof *p));
    p->fd = fd;
    p->pid = pid;
    return p;
}

long sh_pty_read(sh_pty *p, char *buf, size_t n) {
    for (;;) {
        const ssize_t got = read(p->fd, buf, n);
        if (got >= 0) return (long)got;
        if (errno != EINTR) return 0;  // EIO: the other side closed
    }
}

long sh_pty_write(sh_pty *p, const char *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        const ssize_t put = write(p->fd, buf + done, n - done);
        if (put < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += (size_t)put;
    }
    return (long)done;
}

void sh_pty_resize(sh_pty *p, int cols, int rows) {
    const struct winsize size = {(unsigned short)rows, (unsigned short)cols, 0, 0};
    ioctl(p->fd, TIOCSWINSZ, &size);
}

// Every process in the terminal's session: the shell, and all it started that did not leave the session with setsid. An interactive
// shell gives each command a process group of its own, so the shell's group alone misses them: codex, for one, catches the
// hangup and would run on with nobody to talk to. Malloc'd; NULL with *n = 0 when processes cannot be listed here.
static member *session_members(pid_t sid, size_t *n) {
    *n = 0;
    if (sid <= 1) return NULL;
    const pid_t self = getpid();
    size_t cap = 16;
    member *out = need(malloc(cap * sizeof *out));
#if defined(__linux__)
    DIR *d = opendir("/proc");
    if (d) {
        const struct dirent *e;
        while ((e = readdir(d))) {
            char *end;
            const long pid = strtol(e->d_name, &end, 10);
            if (*end || pid <= 1 || pid == self || getsid((pid_t)pid) != sid) continue;
            if (*n == cap) out = need(realloc(out, (cap *= 2) * sizeof *out));
            out[(*n)++] = (member){(pid_t)pid, getpgid((pid_t)pid)};
        }
        closedir(d);
    }
#elif defined(__APPLE__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_SESSION, sid};
    size_t len = 0;
    if (sysctl(mib, 4, NULL, &len, NULL, 0) == 0) {
        len += 32 * sizeof(struct kinfo_proc);  // room for some started since
        struct kinfo_proc *list = need(malloc(len));
        if (sysctl(mib, 4, list, &len, NULL, 0) == 0) {
            for (size_t i = 0; i < len / sizeof *list; ++i) {
                const pid_t pid = list[i].kp_proc.p_pid;
                if (pid <= 1 || pid == self) continue;
                if (*n == cap) out = need(realloc(out, (cap *= 2) * sizeof *out));
                out[(*n)++] = (member){pid, list[i].kp_eproc.e_pgid};
            }
        }
        free(list);
    }
#endif
    return out;
}

// `sig` to the session as it is now and as the hangup found it; a remembered process only while it is still in the same group,
// since its number may have gone to another. Returns how many were signalled.
static int signal_session(sh_pty *p, int sig) {
    if (p->pid <= 1) return 0;
    size_t n = 0;
    member *now = session_members(p->pid, &n);
    int hit = 0;
    for (size_t i = 0; i < n; ++i) hit += kill(now[i].pid, sig) == 0;
    for (size_t i = 0; i < p->n_members; ++i) {
        bool seen = false;
        for (size_t j = 0; j < n && !seen; ++j) seen = now[j].pid == p->members[i].pid;
        if (!seen && getpgid(p->members[i].pid) == p->members[i].pgid) hit += kill(p->members[i].pid, sig) == 0;
    }
    free(now);
    if (!n && !p->n_members) hit += kill(-p->pid, sig) == 0;  // no list of processes: the shell's own group, at least
    return hit;
}

// Those that fork while being killed: a few more rounds, until none is left
static void kill_session(sh_pty *p) {
    for (int round = 0; round < 4 && signal_session(p, SIGKILL) > 0; ++round) usleep(5000);
}

void sh_pty_hangup(sh_pty *p) {
    if (p->pid <= 0) return;
    if (!p->members) p->members = session_members(p->pid, &p->n_members);
    signal_session(p, SIGHUP);
    signal_session(p, SIGCONT);  // a stopped job hears the hangup too
}

void sh_pty_kill(sh_pty *p) {
    if (p->pid > 0) kill_session(p);
}

// how many besides the shell are still in the session (a zombie counts until it is reaped)
static int others_in_session(sh_pty *p) { return signal_session(p, 0) - (kill(p->pid, 0) == 0); }

void sh_pty_free(sh_pty *p) {
    if (!p) return;
    close(p->fd);
    if (p->pid > 0) {
        // A terminal's programs end with it; one that should outlive it leaves the session (setsid). The hangup alone is not enough:
        // macOS revokes the terminal when the shell exits, so a program that ignores the hangup no longer even holds it.
        if (others_in_session(p) > 0) {
            signal_session(p, SIGTERM);
            signal_session(p, SIGCONT);
            for (int i = 0; i < 50 && others_in_session(p) > 0; ++i) usleep(10000);
            kill_session(p);
        }
        int status;
        if (waitpid(p->pid, &status, WNOHANG) == 0) {
            kill_session(p);  // before the shell is reaped: until then its number cannot go to another session
            waitpid(p->pid, &status, 0);
        }
    }
    free(p->members);
    free(p);
}
