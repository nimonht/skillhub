// A report for a bug: what this program is, where it runs, what it holds and how much memory it uses, as text for people.
// Read only: nothing is changed, no lock is taken, nothing leaves the machine. The home folder is written as ~, and user and
// host names are left out. The window adds its own part (src/app.cpp) to the one the background service makes.
#pragma once

#include <stdbool.h>

#include "sh_store.h"

#ifdef __cplusplus
extern "C" {
#endif

// This process's own counters, in kilobytes; -1 where the system does not say. Only this process is looked at: no other process
// is opened, listed or read.
typedef struct {
    long long rss_kb;      // resident: Linux VmRSS, macOS resident size, Windows working set
    long long peak_kb;     // the most it has been
    long long virtual_kb;  // Linux VmSize, macOS virtual size, Windows private (committed) bytes
    long long threads;
    long long handles;     // open files (Linux, macOS) or handles (Windows)
} sh_diag_memory;

bool sh_diag_memory_read(sh_diag_memory *out);
// "54.2 MB resident (peak 60.1 MB), 812.0 MB virtual, 9 threads, 21 handles": what is known, in words
char *sh_diag_memory_text(const sh_diag_memory *m);
// `role` says which program writes it: "background service", "command line". The result is malloc'd.
char *sh_diag_report(sh_store *st, const char *role);
// `text` with the folder `home` written as ~, wherever it occurs (a settings file holds paths)
char *sh_diag_shorten(const char *text, const char *home);

#ifdef __cplusplus
}
#endif
