#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE
#endif

#include "sh_proc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "sh_fs.h"
#include "sh_path.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
#endif

void sh_proc_result_free(sh_proc_result *r) {
    sh_buf_free(&r->out);
    sh_buf_free(&r->err);
}

void sh_path_dirs(sh_strv *out) {
    char *path = sh_getenv("PATH");
    if (!path) return;
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    sh_strv parts = SH_STRV_INIT;
    sh_split(path, strlen(path), sep, true, &parts);
    for (size_t i = 0; i < parts.len; ++i) sh_strv_take(out, sh_path_clean(parts.items[i]));
    sh_strv_free(&parts);
    free(path);
}

char *sh_find_executable(const char *name, const sh_strv *dirs_in) {
    if (!name || !*name) return NULL;
    sh_strv own = SH_STRV_INIT;
    const sh_strv *dirs = dirs_in;
    if (!dirs) {
        sh_path_dirs(&own);
        dirs = &own;
    }
    char *found = NULL;
#ifdef _WIN32
    sh_strv exts = SH_STRV_INIT;
    char *pathext = sh_getenv("PATHEXT");
    sh_split(pathext ? pathext : ".COM;.EXE;.BAT;.CMD", strlen(pathext ? pathext : ".COM;.EXE;.BAT;.CMD"), ';', true, &exts);
    free(pathext);
    const char *dot = strrchr(sh_path_name(name), '.');
    bool has_ext = false;
    for (size_t i = 0; dot && i < exts.len; ++i)
        if (_stricmp(dot, exts.items[i]) == 0) has_ext = true;
    for (size_t i = 0; !found && i < dirs->len; ++i) {
        char *base = sh_path_join(dirs->items[i], name);
        if (has_ext && sh_is_file(base)) found = sh_strdup(base);
        for (size_t k = 0; !found && !has_ext && k < exts.len; ++k) {
            char *candidate = sh_strfmt("%s%s", base, exts.items[k]);
            if (sh_is_file(candidate)) found = candidate;
            else free(candidate);
        }
        free(base);
    }
    sh_strv_free(&exts);
#else
    for (size_t i = 0; !found && i < dirs->len; ++i) {
        char *candidate = sh_path_join(dirs->items[i], name);
        if (sh_is_executable(candidate)) found = candidate;
        else free(candidate);
    }
#endif
    sh_strv_free(&own);
    return found;
}

const char *sh_git_program(void) {
    static char *git;
    if (git) return git;
    sh_strv dirs = SH_STRV_INIT;
    sh_path_dirs(&dirs);
#if defined(__APPLE__)
    sh_strv_push(&dirs, "/opt/homebrew/bin");  // apps started from the Dock have a short PATH
    sh_strv_push(&dirs, "/usr/local/bin");
    sh_strv_push(&dirs, "/usr/bin");
#elif defined(_WIN32)
    static const char *const vars[] = {"ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"};
    for (size_t i = 0; i < 3; ++i) {
        char *v = sh_getenv(vars[i]);
        if (v && *v) {
            char *clean = sh_path_clean(v);
            sh_strv_take(&dirs, sh_strfmt("%s/Git/cmd", clean));
            sh_strv_take(&dirs, sh_strfmt("%s/Programs/Git/cmd", clean));
            free(clean);
        }
        free(v);
    }
#endif
    char *found = sh_find_executable("git", &dirs);
    sh_strv_free(&dirs);
    git = found ? found : sh_strdup("git");
    return git;
}

#ifdef _WIN32

static wchar_t *append_quoted(wchar_t *out, const wchar_t *arg) {
    if (*arg && !wcspbrk(arg, L" \t\n\v\"")) {
        const size_t n = wcslen(arg);
        memcpy(out, arg, n * sizeof *out);
        return out + n;
    }
    *out++ = L'"';
    for (const wchar_t *p = arg;; ++p) {
        size_t backslashes = 0;
        while (*p == L'\\') {
            ++p;
            ++backslashes;
        }
        if (*p == L'\0') {
            for (size_t i = 0; i < backslashes * 2; ++i) *out++ = L'\\';
            break;
        }
        const size_t keep = *p == L'"' ? backslashes * 2 + 1 : backslashes;
        for (size_t i = 0; i < keep; ++i) *out++ = L'\\';
        *out++ = *p;
    }
    *out++ = L'"';
    return out;
}

static wchar_t *command_line(const char *const *argv) {
    size_t size = 1;
    wchar_t **wide = NULL;
    size_t n = 0;
    while (argv[n]) ++n;
    wide = sh_xcalloc(n, sizeof *wide);
    for (size_t i = 0; i < n; ++i) {
        wide[i] = sh_wide(argv[i]);
        size += 2 * wcslen(wide[i]) + 3;
    }
    wchar_t *cmd = sh_xcalloc(size, sizeof *cmd), *end = cmd;
    for (size_t i = 0; i < n; ++i) {
        if (i) *end++ = L' ';
        end = append_quoted(end, wide[i]);
        free(wide[i]);
    }
    *end = 0;
    free(wide);
    return cmd;
}

// the current environment with `add` entries replacing variables of the same name (names compare without case)
static wchar_t *environment(const char *const *add) {
    if (!add || !add[0]) return NULL;
    sh_strv entries = SH_STRV_INIT;
    wchar_t *block = GetEnvironmentStringsW();
    for (wchar_t *e = block; *e; e += wcslen(e) + 1) sh_strv_take(&entries, sh_narrow(e));
    FreeEnvironmentStringsW(block);
    for (size_t k = 0; add[k]; ++k) {
        const char *eq = strchr(add[k], '=');
        if (!eq) continue;
        const size_t name_len = (size_t)(eq - add[k]);
        for (size_t i = 0; i < entries.len; ++i) {
            if (_strnicmp(entries.items[i], add[k], name_len) == 0 && entries.items[i][name_len] == '=') {
                free(entries.items[i]);
                entries.items[i] = entries.items[--entries.len];
                break;
            }
        }
        sh_strv_push(&entries, add[k]);
    }
    sh_buf b = SH_BUF_INIT;
    for (size_t i = 0; i < entries.len; ++i) {
        wchar_t *w = sh_wide(entries.items[i]);
        sh_buf_append(&b, w, (wcslen(w) + 1) * sizeof *w);
        free(w);
    }
    const wchar_t zero = 0;
    sh_buf_append(&b, &zero, sizeof zero);
    sh_strv_free(&entries);
    return (wchar_t *)sh_buf_steal(&b);
}

typedef struct {
    HANDLE h;
    const void *data;
    size_t len;
    sh_buf *sink;
} pipe_job;

static DWORD WINAPI writer(void *arg) {
    pipe_job *j = arg;
    const char *p = j->data;
    size_t left = j->len;
    while (left > 0) {
        DWORD w = 0;
        if (!WriteFile(j->h, p, left > 65536 ? 65536 : (DWORD)left, &w, NULL)) break;
        p += w;
        left -= w;
    }
    CloseHandle(j->h);
    return 0;
}

static DWORD WINAPI reader(void *arg) {
    pipe_job *j = arg;
    char buf[65536];
    DWORD n = 0;
    while (ReadFile(j->h, buf, sizeof buf, &n, NULL) && n > 0) sh_buf_append(j->sink, buf, n);
    return 0;
}

bool sh_proc_run(const char *const *argv, const sh_proc_opts *opts, sh_proc_result *r) {
    static const sh_proc_opts none = {0};
    const sh_proc_opts *o = opts ? opts : &none;
    memset(r, 0, sizeof *r);
    r->exit_code = -1;
    sh_buf_append(&r->out, "", 0);
    sh_buf_append(&r->err, "", 0);

    char *program = sh_path_is_absolute(argv[0]) ? sh_strdup(argv[0]) : sh_find_executable(argv[0], NULL);
    if (!program) return false;

    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL, err_r = NULL, err_w = NULL;
    if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0)) {
        free(program);
        return false;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    if (o->input) {
        CreatePipe(&in_r, &in_w, &sa, 0);
        SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    } else {
        in_r = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    }

    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = err_w;
    PROCESS_INFORMATION pi;
    wchar_t *wprog = sh_wide_path(program), *cmd = command_line(argv), *env = environment(o->env);
    wchar_t *wcwd = o->cwd ? sh_wide_path(o->cwd) : NULL;
    const BOOL ok = CreateProcessW(wprog, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env, wcwd, &si, &pi);
    free(wprog);
    free(cmd);
    free(env);
    free(wcwd);
    free(program);
    CloseHandle(out_w);
    CloseHandle(err_w);
    if (in_r) CloseHandle(in_r);
    if (!ok) {
        CloseHandle(out_r);
        CloseHandle(err_r);
        if (in_w) CloseHandle(in_w);
        return false;
    }
    r->started = true;
    CloseHandle(pi.hThread);

    pipe_job wj = {in_w, o->input, o->input_len, NULL}, ej = {err_r, NULL, 0, &r->err}, oj = {out_r, NULL, 0, &r->out};
    HANDLE wt = in_w ? CreateThread(NULL, 0, writer, &wj, 0, NULL) : NULL;
    HANDLE et = CreateThread(NULL, 0, reader, &ej, 0, NULL);
    HANDLE ot = CreateThread(NULL, 0, reader, &oj, 0, NULL);
    if (WaitForSingleObject(pi.hProcess, o->timeout_ms > 0 ? (DWORD)o->timeout_ms : INFINITE) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
        r->timed_out = true;
    }
    if (wt) {
        WaitForSingleObject(wt, INFINITE);
        CloseHandle(wt);
    }
    WaitForSingleObject(ot, INFINITE);
    WaitForSingleObject(et, INFINITE);
    CloseHandle(ot);
    CloseHandle(et);
    CloseHandle(out_r);
    CloseHandle(err_r);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    r->exit_code = (int)code;
    return !r->timed_out && code == 0;
}

bool sh_proc_detach(const char *const *argv) {
    char *program = sh_path_is_absolute(argv[0]) ? sh_strdup(argv[0]) : sh_find_executable(argv[0], NULL);
    if (!program) return false;
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    wchar_t *wprog = sh_wide_path(program), *cmd = command_line(argv);
    const BOOL ok = CreateProcessW(wprog, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL, &si, &pi);
    free(wprog);
    free(cmd);
    free(program);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return ok != 0;
}

#else  // POSIX

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

static void close_quietly(int *fd) {
    if (*fd >= 0) close(*fd);
    *fd = -1;
}

bool sh_proc_run(const char *const *argv, const sh_proc_opts *opts, sh_proc_result *r) {
    static const sh_proc_opts none = {0};
    const sh_proc_opts *o = opts ? opts : &none;
    memset(r, 0, sizeof *r);
    r->exit_code = -1;
    sh_buf_append(&r->out, "", 0);
    sh_buf_append(&r->err, "", 0);

    int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1}, report[2] = {-1, -1};
    if (pipe(in) || pipe(out) || pipe(err) || pipe(report)) {
        close_quietly(&in[0]); close_quietly(&in[1]); close_quietly(&out[0]); close_quietly(&out[1]);
        close_quietly(&err[0]); close_quietly(&err[1]); close_quietly(&report[0]); close_quietly(&report[1]);
        return false;
    }
    fcntl(report[1], F_SETFD, FD_CLOEXEC);  // closes on a successful exec; carries errno when exec fails

    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(err[1], 2);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]); close(err[0]); close(err[1]); close(report[0]);
        signal(SIGPIPE, SIG_DFL);
        if (o->cwd && chdir(o->cwd) != 0) {
            const int e = errno;
            if (write(report[1], &e, sizeof e) < 0) {}
            _exit(127);
        }
        for (size_t i = 0; o->env && o->env[i]; ++i) putenv((char *)o->env[i]);
        if (strchr(argv[0], '/')) execv(argv[0], (char *const *)argv);
        else execvp(argv[0], (char *const *)argv);
        const int e = errno;
        if (write(report[1], &e, sizeof e) < 0) {}
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    close(err[1]);
    close(report[1]);
    int exec_errno = 0;
    const ssize_t got = read(report[0], &exec_errno, sizeof exec_errno);
    close(report[0]);
    if (got == (ssize_t)sizeof exec_errno) {  // exec failed: the program never ran
        close(in[1]);
        close(out[0]);
        close(err[0]);
        waitpid(pid, NULL, 0);
        return false;
    }
    r->started = true;

    const char *input = o->input;
    size_t input_left = o->input ? o->input_len : 0;
    if (input_left == 0) close_quietly(&in[1]);
    else set_nonblock(in[1]);
    set_nonblock(out[0]);
    set_nonblock(err[0]);
    const int64_t deadline = o->timeout_ms > 0 ? now_ms() + o->timeout_ms : 0;
    char buf[65536];
    while (out[0] >= 0 || err[0] >= 0) {
        struct pollfd fds[3];
        int nfds = 0, io = -1, ie = -1, ii = -1;
        if (out[0] >= 0) { io = nfds; fds[nfds].fd = out[0]; fds[nfds++].events = POLLIN; }
        if (err[0] >= 0) { ie = nfds; fds[nfds].fd = err[0]; fds[nfds++].events = POLLIN; }
        if (in[1] >= 0) { ii = nfds; fds[nfds].fd = in[1]; fds[nfds++].events = POLLOUT; }
        int wait = -1;
        if (deadline) {
            wait = (int)(deadline - now_ms());
            if (wait <= 0) {
                kill(pid, SIGKILL);
                r->timed_out = true;
                break;
            }
        }
        const int n = poll(fds, (nfds_t)nfds, wait);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) break;
        if (ii >= 0 && (fds[ii].revents & (POLLOUT | POLLERR | POLLHUP))) {
            const ssize_t w = write(in[1], input, input_left > sizeof buf ? sizeof buf : input_left);
            if (w > 0) {
                input += w;
                input_left -= (size_t)w;
            }
            if (w < 0 && errno != EAGAIN && errno != EINTR) input_left = 0;  // the program closed its stdin
            if (input_left == 0) close_quietly(&in[1]);
        }
        if (io >= 0 && (fds[io].revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t m = read(out[0], buf, sizeof buf);
            if (m > 0) sh_buf_append(&r->out, buf, (size_t)m);
            else if (m == 0 || (errno != EAGAIN && errno != EINTR)) close_quietly(&out[0]);
        }
        if (ie >= 0 && (fds[ie].revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t m = read(err[0], buf, sizeof buf);
            if (m > 0) sh_buf_append(&r->err, buf, (size_t)m);
            else if (m == 0 || (errno != EAGAIN && errno != EINTR)) close_quietly(&err[0]);
        }
    }
    close_quietly(&in[1]);
    close_quietly(&out[0]);
    close_quietly(&err[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) r->exit_code = WEXITSTATUS(status);
    return !r->timed_out && r->exit_code == 0;
}

bool sh_proc_detach(const char *const *argv) {
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        // a grandchild runs the program, so nobody has to wait for it
        if (fork() == 0) {
            setsid();
            const int null = open("/dev/null", O_RDWR);
            if (null >= 0) {
                dup2(null, 0);
                dup2(null, 1);
                dup2(null, 2);
            }
            if (strchr(argv[0], '/')) execv(argv[0], (char *const *)argv);
            else execvp(argv[0], (char *const *)argv);
            _exit(127);
        }
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    return true;
}

#endif
