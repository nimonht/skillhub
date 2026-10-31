// Time stamps in the formats Skill Hub writes, and short sleeps.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

char *sh_now_iso(void);      // "2026-10-25T09:20:41.715Z" (UTC, milliseconds), as JavaScript's toISOString writes it
char *sh_stamp(void);        // "20261025-162041" in local time: backup folder names
int64_t sh_now_ms(void);     // wall clock, milliseconds since 1970
int64_t sh_mono_ms(void);    // monotonic clock, for timeouts
void sh_sleep_ms(int ms);
char *sh_local_hms(void);    // "16:20:41", for log lines

#ifdef __cplusplus
}
#endif
