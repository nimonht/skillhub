// skillhub: the command line. With no command, or `gui`, it starts the window (skillhub-gui, skillhub.exe on Windows),
// which sits next to it. Until the C core replaces it (milestone M1 of the 2.0 plan), every other command also runs in
// the window's program, which still carries the 1.x command line; only --version is answered here.
#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh_exe.h"

#ifndef SKILLHUB_VERSION
#define SKILLHUB_VERSION "dev"
#endif

#ifdef _WIN32

#include <windows.h>
#include <wchar.h>

#define GUI_NAME L"skillhub.exe"

// One argument for CreateProcess, quoted the way CommandLineToArgvW and the C runtime split it again.
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
        if (*p == L'\0') {  // double the backslashes before the closing quote
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

static void inherit(DWORD which, HANDLE *slot) {
    HANDLE h = GetStdHandle(which);
    if (h && h != INVALID_HANDLE_VALUE) SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    *slot = h;
}

// Windows has no exec: start the window's program with the same standard handles, wait, and pass its exit code on.
static int forward(int argc, wchar_t **argv) {
    static wchar_t gui[32768];
    const wchar_t *override = _wgetenv(L"SKILLHUB_GUI_BIN");
    if (override && *override) {
        if (wcslen(override) >= sizeof gui / sizeof gui[0]) return 127;
        wcscpy(gui, override);
    } else {
        const DWORD n = GetModuleFileNameW(NULL, gui, (DWORD)(sizeof gui / sizeof gui[0]));
        wchar_t *slash = n ? wcsrchr(gui, L'\\') : NULL;
        if (!slash || (size_t)(slash - gui) + 1 + wcslen(GUI_NAME) >= sizeof gui / sizeof gui[0]) {
            fputs("skillhub: cannot find its own location\n", stderr);
            return 127;
        }
        wcscpy(slash + 1, GUI_NAME);
    }

    size_t size = 2 * wcslen(gui) + 3;
    for (int i = 1; i < argc; ++i) size += 2 * wcslen(argv[i]) + 3;
    wchar_t *cmd = calloc(size + 1, sizeof *cmd);
    if (!cmd) return 127;
    wchar_t *end = append_quoted(cmd, gui);
    for (int i = 1; i < argc; ++i) {
        *end++ = L' ';
        end = append_quoted(end, argv[i]);
    }
    *end = L'\0';

    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    inherit(STD_INPUT_HANDLE, &si.hStdInput);
    inherit(STD_OUTPUT_HANDLE, &si.hStdOutput);
    inherit(STD_ERROR_HANDLE, &si.hStdError);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(gui, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fwprintf(stderr, L"skillhub: cannot start %ls (error %lu)\n", gui, (unsigned long)GetLastError());
        free(cmd);
        return 127;
    }
    free(cmd);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}

int wmain(int argc, wchar_t **argv) {
    if (argc > 1 && (wcscmp(argv[1], L"--version") == 0 || wcscmp(argv[1], L"version") == 0)) {
        printf("skillhub %s\n", SKILLHUB_VERSION);
        return 0;
    }
    return forward(argc, argv);
}

#else  // POSIX

#include <unistd.h>

#define GUI_NAME "skillhub-gui"

static int forward(int argc, char **argv) {
    char gui[4096];
    const char *override = getenv("SKILLHUB_GUI_BIN");
    if (override && *override) {
        if (strlen(override) >= sizeof gui) return 127;
        strcpy(gui, override);
    } else {
        // the real location: a link such as /usr/local/bin/skillhub -> the macOS bundle must not decide where to look
        const size_t n = sh_exe_dir(gui, sizeof gui);
        if (n == 0 || n + 1 + sizeof GUI_NAME > sizeof gui) {
            fputs("skillhub: cannot find its own location\n", stderr);
            return 127;
        }
        gui[n] = '/';
        memcpy(gui + n + 1, GUI_NAME, sizeof GUI_NAME);
    }
    char **args = calloc((size_t)argc + 1, sizeof *args);
    if (!args) return 127;
    args[0] = gui;
    for (int i = 1; i < argc; ++i) args[i] = argv[i];
    execv(gui, args);
    fprintf(stderr, "skillhub: cannot start %s: %s\n", gui, strerror(errno));
    free(args);
    return 127;
}

int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "version") == 0)) {
        printf("skillhub %s\n", SKILLHUB_VERSION);
        return 0;
    }
    return forward(argc, argv);
}

#endif
