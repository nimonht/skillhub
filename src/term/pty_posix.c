#if !defined(__APPLE__)
#define _GNU_SOURCE  // ptsname_r
#endif

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

#include "sh_pty.h"

extern char **environ;

struct sh_pty {
    int fd;
    pid_t pid;
};

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
    char **envp = calloc(n_old + n_new + 1, sizeof *envp);
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
    sh_pty *p = calloc(1, sizeof *p);
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

void sh_pty_hangup(sh_pty *p) {
    if (p->pid > 0) kill(-p->pid, SIGHUP);
}

void sh_pty_free(sh_pty *p) {
    if (!p) return;
    close(p->fd);
    if (p->pid > 0) {
        int status;
        if (waitpid(p->pid, &status, WNOHANG) == 0) {
            kill(-p->pid, SIGKILL);
            waitpid(p->pid, &status, 0);
        }
    }
    free(p);
}
