#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE
#endif

#include "sh_time.h"

#include <time.h>

#include "sh_str.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#include <unistd.h>
#endif

int64_t sh_now_ms(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int64_t)(t / 10000 - 11644473600000ULL);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

int64_t sh_mono_ms(void) {
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

void sh_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

static void broken_down(int64_t ms, bool utc, struct tm *out) {
    const time_t t = (time_t)(ms / 1000);
#ifdef _WIN32
    if (utc) gmtime_s(out, &t);
    else localtime_s(out, &t);
#else
    if (utc) gmtime_r(&t, out);
    else localtime_r(&t, out);
#endif
}

char *sh_now_iso(void) {
    const int64_t ms = sh_now_ms();
    struct tm tm;
    broken_down(ms, true, &tm);
    return sh_strfmt("%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                     tm.tm_sec, (int)(ms % 1000));
}

char *sh_stamp(void) {
    struct tm tm;
    broken_down(sh_now_ms(), false, &tm);
    return sh_strfmt("%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

char *sh_local_hms(void) {
    struct tm tm;
    broken_down(sh_now_ms(), false, &tm);
    return sh_strfmt("%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}
