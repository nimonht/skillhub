#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "sh_pty.h"

// ConPTY arrived with Windows 10 1809; looked up by name so the program still starts on anything older
typedef HRESULT(WINAPI *create_fn)(COORD, HANDLE, HANDLE, DWORD, void **);
typedef HRESULT(WINAPI *resize_fn)(void *, COORD);
typedef void(WINAPI *close_fn)(void *);

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

struct sh_pty {
    void *console;
    HANDLE in, out, process;
    close_fn close_console;
    resize_fn resize;
};

static wchar_t *wide(const char *s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = calloc((size_t)n + 1, sizeof *w);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char *message(const char *what) {
    char *m = malloc(128);
    if (m) snprintf(m, 128, "%s (error %lu)", what, GetLastError());
    return m;
}

// quoted the way CommandLineToArgvW splits it again
static void quote(wchar_t *out, size_t *at, const wchar_t *arg) {
    out[(*at)++] = L'"';
    for (const wchar_t *c = arg;; ++c) {
        size_t slashes = 0;
        while (*c == L'\\') {
            ++c;
            ++slashes;
        }
        const size_t doubled = !*c ? slashes * 2 : *c == L'"' ? slashes * 2 + 1 : slashes;
        for (size_t i = 0; i < doubled; ++i) out[(*at)++] = L'\\';
        if (!*c) break;
        out[(*at)++] = *c;
    }
    out[(*at)++] = L'"';
}

sh_pty *sh_pty_spawn(const char *program, const char *const *argv, const char *cwd, const char *const *env, int cols, int rows, char **err) {
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    const create_fn create = (create_fn)(void *)GetProcAddress(kernel, "CreatePseudoConsole");
    if (!create) {
        if (err) *err = _strdup("the built-in terminal needs Windows 10 1809 or newer");
        return NULL;
    }
    HANDLE in_read, in_write, out_read, out_write;
    if (!CreatePipe(&in_read, &in_write, NULL, 0) || !CreatePipe(&out_read, &out_write, NULL, 0)) {
        if (err) *err = message("cannot make pipes");
        return NULL;
    }
    const COORD size = {(SHORT)cols, (SHORT)rows};
    void *console = NULL;
    if (FAILED(create(size, in_read, out_write, 0, &console))) {
        if (err) *err = message("cannot open a console");
        return NULL;
    }
    CloseHandle(in_read);
    CloseHandle(out_write);

    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    si.lpAttributeList = malloc(bytes);
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &bytes);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console, sizeof console, NULL, NULL);

    size_t room = 16;
    for (size_t i = 0; argv[i]; ++i) room += 3 * strlen(argv[i]) + 4;
    wchar_t *line = calloc(room, sizeof *line);
    size_t at = 0;
    for (size_t i = 0; argv[i]; ++i) {
        wchar_t *w = wide(argv[i]);
        if (i) line[at++] = L' ';
        quote(line, &at, w);
        free(w);
    }
    for (size_t i = 0; env && env[i]; ++i) {
        const char *eq = strchr(env[i], '=');
        if (!eq) continue;
        char *name = _strdup(env[i]);
        name[eq - env[i]] = 0;
        wchar_t *wn = wide(name), *wv = wide(eq + 1);
        SetEnvironmentVariableW(wn, wv);  // inherited by the child; this process has no use for the difference
        free(wn);
        free(wv);
        free(name);
    }
    wchar_t *wprog = wide(program), *wcwd = cwd && *cwd ? wide(cwd) : NULL;
    PROCESS_INFORMATION pi;
    const BOOL ok = CreateProcessW(wprog, line, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, NULL, wcwd, &si.StartupInfo, &pi);
    free(line);
    free(wprog);
    free(wcwd);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
    if (!ok) {
        if (err) *err = message("cannot start the shell");
        ((close_fn)(void *)GetProcAddress(kernel, "ClosePseudoConsole"))(console);
        CloseHandle(in_write);
        CloseHandle(out_read);
        return NULL;
    }
    CloseHandle(pi.hThread);
    sh_pty *p = calloc(1, sizeof *p);
    p->console = console;
    p->in = in_write;
    p->out = out_read;
    p->process = pi.hProcess;
    p->close_console = (close_fn)(void *)GetProcAddress(kernel, "ClosePseudoConsole");
    p->resize = (resize_fn)(void *)GetProcAddress(kernel, "ResizePseudoConsole");
    return p;
}

long sh_pty_read(sh_pty *p, char *buf, size_t n) {
    DWORD got = 0;
    return ReadFile(p->out, buf, (DWORD)n, &got, NULL) ? (long)got : 0;
}

long sh_pty_write(sh_pty *p, const char *buf, size_t n) {
    DWORD put = 0;
    return WriteFile(p->in, buf, (DWORD)n, &put, NULL) ? (long)put : -1;
}

void sh_pty_resize(sh_pty *p, int cols, int rows) {
    const COORD size = {(SHORT)cols, (SHORT)rows};
    if (p->console) p->resize(p->console, size);
}

void sh_pty_hangup(sh_pty *p) {
    if (p->console) {
        p->close_console(p->console);  // the shell goes, and the read pipe ends
        p->console = NULL;
    }
}

void sh_pty_free(sh_pty *p) {
    if (!p) return;
    sh_pty_hangup(p);
    CloseHandle(p->in);
    CloseHandle(p->out);
    if (WaitForSingleObject(p->process, 500) == WAIT_TIMEOUT) TerminateProcess(p->process, 1);
    CloseHandle(p->process);
    free(p);
}
