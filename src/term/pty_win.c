#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <tlhelp32.h>

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
    HANDLE job;  // the shell and everything it starts; NULL when Windows would not make one
    close_fn close_console;
    resize_fn resize;
};

// out of memory ends the program, as sh_xmalloc does in the core, which is not linked here
static void *need(void *p) {
    if (!p) abort();
    return p;
}

static wchar_t *wide(const char *s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = need(calloc((size_t)n + 1, sizeof *w));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char *message(const char *what) {
    char *m = malloc(128);
    if (m) snprintf(m, 128, "%s (error %lu)", what, GetLastError());
    return m;
}

// quoted the way CommandLineToArgvW splits it again; plain words stay bare, because cmd.exe takes no quoted /K
static void quote(wchar_t *out, size_t *at, const wchar_t *arg) {
    if (*arg && !wcspbrk(arg, L" \t\"")) {
        while (*arg) out[(*at)++] = *arg++;
        return;
    }
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
    HANDLE in_read = NULL, in_write = NULL, out_read = NULL, out_write = NULL;
    if (!CreatePipe(&in_read, &in_write, NULL, 0) || !CreatePipe(&out_read, &out_write, NULL, 0)) {
        if (err) *err = message("cannot make pipes");
        if (in_read) CloseHandle(in_read);
        if (in_write) CloseHandle(in_write);
        return NULL;
    }
    const COORD size = {(SHORT)cols, (SHORT)rows};
    void *console = NULL;
    if (FAILED(create(size, in_read, out_write, 0, &console))) {
        if (err) *err = message("cannot open a console");
        CloseHandle(in_read);
        CloseHandle(in_write);
        CloseHandle(out_read);
        CloseHandle(out_write);
        return NULL;
    }
    CloseHandle(in_read);
    CloseHandle(out_write);

    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;  // with none given: otherwise a redirected parent hands the shell its own stdio, and the console stays empty
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    si.lpAttributeList = need(malloc(bytes));
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &bytes);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console, sizeof console, NULL, NULL);

    size_t room = 16;
    for (size_t i = 0; argv[i]; ++i) room += 3 * strlen(argv[i]) + 4;
    wchar_t *line = need(calloc(room, sizeof *line));
    size_t at = 0;
    for (size_t i = 0; argv[i]; ++i) {
        wchar_t *w = wide(argv[i]);
        if (i) line[at++] = L' ';
        quote(line, &at, w);
        free(w);
    }
    // the child inherits these, and only for this start: what this process runs later must not seem to run inside the terminal
    size_t vars = 0;
    while (env && env[vars]) ++vars;
    wchar_t **names = need(calloc(vars + 1, sizeof *names)), **kept = need(calloc(vars + 1, sizeof *kept));
    for (size_t i = 0; i < vars; ++i) {
        const char *eq = strchr(env[i], '=');
        if (!eq) continue;
        char *name = need(_strdup(env[i]));
        name[eq - env[i]] = 0;
        names[i] = wide(name);
        free(name);
        const DWORD len = GetEnvironmentVariableW(names[i], NULL, 0);
        if (len > 0) {
            kept[i] = need(malloc(len * sizeof **kept));
            GetEnvironmentVariableW(names[i], kept[i], len);
        }
        wchar_t *value = wide(eq + 1);
        SetEnvironmentVariableW(names[i], value);
        free(value);
    }
    // A job holds what the shell starts, so closing the terminal can find it: programs that start helpers with hidden consoles of
    // their own (codex, node) leave those running when the terminal's console closes. A program may still leave the job on purpose.
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
        memset(&limits, 0, sizeof limits);
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    }
    wchar_t *wprog = wide(program), *wcwd = cwd && *cwd ? wide(cwd) : NULL;
    PROCESS_INFORMATION pi;
    const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | (job ? CREATE_SUSPENDED : 0);  // in the job before it starts anything
    const BOOL ok = CreateProcessW(wprog, line, NULL, NULL, FALSE, flags, NULL, wcwd, &si.StartupInfo, &pi);
    const DWORD failure = GetLastError();
    if (ok && job) {
        if (!AssignProcessToJobObject(job, pi.hProcess)) {  // a terminal without the job still works: it ends the shell alone, as before
            CloseHandle(job);
            job = NULL;
        }
        ResumeThread(pi.hThread);  // whatever happened to the job: a shell left suspended would be a dead terminal
    }
    for (size_t i = 0; i < vars; ++i) {
        if (names[i]) SetEnvironmentVariableW(names[i], kept[i]);  // NULL: it was not set
        free(names[i]);
        free(kept[i]);
    }
    free(names);
    free(kept);
    SetLastError(failure);
    free(line);
    free(wprog);
    free(wcwd);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
    if (!ok) {
        if (err) *err = message("cannot start the shell");
        if (job) CloseHandle(job);
        ((close_fn)(void *)GetProcAddress(kernel, "ClosePseudoConsole"))(console);
        CloseHandle(in_write);
        CloseHandle(out_read);
        return NULL;
    }
    CloseHandle(pi.hThread);
    sh_pty *p = need(calloc(1, sizeof *p));
    p->console = console;
    p->in = in_write;
    p->out = out_read;
    p->process = pi.hProcess;
    p->job = job;
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
        p->close_console(p->console);  // the shell and the programs on its console go, and the read pipe ends
        p->console = NULL;
    }
}

// From the program's own file: IMAGE_SUBSYSTEM_WINDOWS_CUI for a console program, _GUI for one with windows; 0 when unknown
static WORD subsystem_of(HANDLE process) {
    wchar_t path[1024];
    DWORD len = sizeof path / sizeof *path;
    if (!QueryFullProcessImageNameW(process, 0, path, &len)) return 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    WORD subsystem = 0;
    IMAGE_DOS_HEADER dos;
    DWORD got = 0;
    if (ReadFile(f, &dos, sizeof dos, &got, NULL) && got == sizeof dos && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0) {
        // the signature, the file header and the optional header as far as Subsystem, which sits at the same place in PE32 and PE32+
        unsigned char nt[4 + sizeof(IMAGE_FILE_HEADER) + 72];
        LARGE_INTEGER at;
        at.QuadPart = dos.e_lfanew;
        if (SetFilePointerEx(f, at, NULL, FILE_BEGIN) && ReadFile(f, nt, sizeof nt, &got, NULL) && got == sizeof nt) {
            DWORD signature;
            memcpy(&signature, nt, sizeof signature);
            if (signature == IMAGE_NT_SIGNATURE) memcpy(&subsystem, nt + 4 + sizeof(IMAGE_FILE_HEADER) + 68, sizeof subsystem);
        }
    }
    CloseHandle(f);
    return subsystem;
}

static bool started_before(HANDLE older, HANDLE younger) {
    FILETIME a, b, unused1, unused2, unused3;
    if (!GetProcessTimes(older, &a, &unused1, &unused2, &unused3) || !GetProcessTimes(younger, &b, &unused1, &unused2, &unused3)) return false;
    return CompareFileTime(&a, &b) <= 0;
}

typedef struct {
    DWORD pid, parent;
} proc_link;

// Started, at any depth, by a program with windows that is still in the job: that app's own, such as the shells in VS Code's
// terminal after `code .`, which go on with it. Each step up must have started earlier, or the number went to another process.
static bool under_windowed_app(HANDLE job, const proc_link *links, size_t n, DWORD pid, HANDLE process) {
    DWORD at = pid;
    for (int depth = 0; depth < 32; ++depth) {
        DWORD parent = 0;
        for (size_t i = 0; i < n && !parent; ++i)
            if (links[i].pid == at) parent = links[i].parent;
        if (!parent || parent == at) return false;
        HANDLE up = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent);
        if (!up) return false;
        BOOL in = FALSE;
        const bool ours = IsProcessInJob(up, job, &in) && in && started_before(up, process);
        const WORD subsystem = ours ? subsystem_of(up) : 0;
        CloseHandle(up);
        if (!ours) return false;   // out of the job: the terminal itself started it
        if (subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI) return true;
        at = parent;
    }
    return false;
}

// The console programs still in the job: helpers on hidden consoles, and programs that would not close with the console. Programs
// with windows of their own stay, and so does what they started.
static void end_console_programs(HANDLE job) {
    if (!job) return;
    const DWORD room = 1024;
    JOBOBJECT_BASIC_PROCESS_ID_LIST *list = malloc(sizeof *list + room * sizeof(ULONG_PTR));
    if (!list) return;
    for (int round = 0; round < 4; ++round) {  // those started while others were ended: a few more rounds
        memset(list, 0, sizeof *list);
        list->NumberOfAssignedProcesses = room;
        if (!QueryInformationJobObject(job, JobObjectBasicProcessIdList, list, (DWORD)(sizeof *list + room * sizeof(ULONG_PTR)), NULL) &&
            GetLastError() != ERROR_MORE_DATA)
            break;
        // who started whom, for every process now: the job knows its members, not their parents
        size_t n = 0, cap = 0;
        proc_link *links = NULL;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W e;
            e.dwSize = sizeof e;
            for (BOOL more = Process32FirstW(snap, &e); more; more = Process32NextW(snap, &e)) {
                if (n == cap) {
                    cap = cap ? cap * 2 : 512;
                    links = need(realloc(links, cap * sizeof *links));
                }
                links[n].pid = e.th32ProcessID;
                links[n].parent = e.th32ParentProcessID;
                ++n;
            }
            CloseHandle(snap);
        }
        const ULONG_PTR *ids = list->ProcessIdList;
        int ended = 0;
        for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i) {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, (DWORD)ids[i]);
            if (!h) continue;
            BOOL in = FALSE;  // the number may have gone to another process since the list was made
            if (IsProcessInJob(h, job, &in) && in && subsystem_of(h) == IMAGE_SUBSYSTEM_WINDOWS_CUI &&
                !under_windowed_app(job, links, n, (DWORD)ids[i], h) && TerminateProcess(h, 1)) {
                WaitForSingleObject(h, 200);
                ++ended;
            }
            CloseHandle(h);
        }
        free(links);
        if (!ended) break;
    }
    free(list);
}

void sh_pty_kill(sh_pty *p) {
    TerminateProcess(p->process, 1);
    end_console_programs(p->job);
}

void sh_pty_free(sh_pty *p) {
    if (!p) return;
    sh_pty_hangup(p);
    CloseHandle(p->in);
    CloseHandle(p->out);
    if (WaitForSingleObject(p->process, 500) == WAIT_TIMEOUT) TerminateProcess(p->process, 1);
    CloseHandle(p->process);
    if (p->job) {
        end_console_programs(p->job);
        CloseHandle(p->job);
    }
    free(p);
}
