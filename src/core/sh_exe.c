#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE  // readlink, realpath
#endif

#include "sh_exe.h"

#include <string.h>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#elif defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif

#ifdef _WIN32

size_t sh_exe_path(char *buf, size_t size) {
    wchar_t raw[32768];
    const DWORD n = GetModuleFileNameW(NULL, raw, (DWORD)(sizeof raw / sizeof raw[0]));
    if (n == 0 || n >= sizeof raw / sizeof raw[0]) return 0;
    // follow a symlink to the program itself, as POSIX realpath would
    wchar_t final[32768];
    const wchar_t *path = raw;
    HANDLE h = CreateFileW(raw, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        const DWORD m = GetFinalPathNameByHandleW(h, final, (DWORD)(sizeof final / sizeof final[0]), FILE_NAME_NORMALIZED);
        CloseHandle(h);
        if (m > 0 && m < sizeof final / sizeof final[0]) {
            path = final;
            if (wcsncmp(path, L"\\\\?\\UNC\\", 8) == 0) {  // \\?\UNC\server\share -> \\server\share
                final[6] = L'\\';
                path = final + 6;
            } else if (wcsncmp(path, L"\\\\?\\", 4) == 0) {
                path = final + 4;
            }
        }
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, buf, (int)size, NULL, NULL);
    return len > 0 ? (size_t)len - 1 : 0;
}

#elif defined(__APPLE__)

size_t sh_exe_path(char *buf, size_t size) {
    char raw[PATH_MAX];
    uint32_t n = sizeof raw;
    if (_NSGetExecutablePath(raw, &n) != 0) return 0;
    char real[PATH_MAX];
    if (!realpath(raw, real)) return 0;
    const size_t len = strlen(real);
    if (len + 1 > size) return 0;
    memcpy(buf, real, len + 1);
    return len;
}

#elif defined(__FreeBSD__)

size_t sh_exe_path(char *buf, size_t size) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    size_t n = size;
    if (sysctl(mib, 4, buf, &n, NULL, 0) != 0 || n == 0) return 0;
    return strlen(buf);
}

#else  // Linux and others with /proc

size_t sh_exe_path(char *buf, size_t size) {
    if (size < 2) return 0;
    const ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n <= 0 || (size_t)n >= size - 1) return 0;  // a full buffer may mean the path was cut
    buf[n] = '\0';
    return (size_t)n;
}

#endif

size_t sh_exe_dir(char *buf, size_t size) {
    size_t n = sh_exe_path(buf, size);
    while (n > 0 && buf[n - 1] != '/'
#ifdef _WIN32
           && buf[n - 1] != '\\'
#endif
    )
        --n;
    if (n == 0) return 0;
    buf[--n] = '\0';  // drop the separator
    return n;
}
