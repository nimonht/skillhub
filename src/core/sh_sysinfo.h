// Facts about this machine, for `skillhub fetch` and `system.info`. Read from the system, never from the network.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *user, *host;
    char *os;        // "CachyOS x86_64"
    char *kernel;    // "Linux 7.3.0-4-cachyos"
    char *shell;     // "fish"
    char *terminal;  // "kitty"
    char *uptime;    // "3 h 12 min"
} sh_sysinfo;

void sh_sysinfo_read(sh_sysinfo *out);
void sh_sysinfo_free(sh_sysinfo *s);

#ifdef __cplusplus
}
#endif
