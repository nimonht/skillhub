// Security reports from skills.sh for public GitHub skills: what three outside scanners (Gen Agent Trust Hub, Socket, Snyk) said
// about a skill, each with its own result and date. Off unless the user turns them on (settings "security.reports"), and off whatever
// the setting says when DO_NOT_TRACK or DISABLE_TELEMETRY is set, because a lookup tells skills.sh which skills someone has.
//
// A repository counts as public only when an anonymous `git ls-remote` of github.com/<owner>/<repo>.git succeeds with every user
// configuration and credential helper switched off; nothing about a private repository is ever sent. A report describes the skill
// upstream on its date, not necessarily the version installed, and the scanners often disagree, so they are never added up.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sh_store.h"
#include "yyjson.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SH_REPORT_OFF,         // the user has not turned reports on
    SH_REPORT_NOT_GITHUB,  // only GitHub skills have reports
    SH_REPORT_PRIVATE,     // the repository is not public: nothing was sent
    SH_REPORT_FAILED,      // skills.sh could not be reached
    SH_REPORT_NONE,        // skills.sh has no report for this skill
    SH_REPORT_OK,
} sh_report_state;

typedef struct {
    const char *scanner;  // "ath", "socket", "snyk" (static)
    const char *label;    // "Gen Agent Trust Hub", "Socket", "Snyk" (static)
    char *risk;           // safe, low, medium, high, critical or unknown
    int alerts;           // -1 when not given
    char *analyzed_at;    // as skills.sh wrote it, "" when not given
} sh_report_row;

typedef struct {
    sh_report_state state;
    sh_report_row rows[3];
    size_t n;
    char *source;  // owner/repo that was asked about, "" when none
} sh_report;

bool sh_reports_enabled(void);   // the setting, unless the environment opts out
bool sh_reports_opted_out(void); // DO_NOT_TRACK or DISABLE_TELEMETRY

// The reports of `names` (skill names) of one skill source, from the cache when it is under a day old. `consent`: look up even
// when the setting is off (the CLI's --reports); the environment's opt-out still wins.
void sh_reports_get(const char *cache, const sh_source *src, const char *const *names, size_t n, bool consent, sh_report *out);
void sh_report_free(sh_report *r);
void sh_report_json(yyjson_mut_doc *d, yyjson_mut_val *o, const sh_report *r);
const char *sh_report_state_name(sh_report_state s);

// The baseline in state.json: what the scanners said when the skill was last installed or updated. Call with the mutation lock held.
// True, with `why`, when some scanner's result got worse since then; with no baseline yet, `now` becomes it.
bool sh_reports_worse_than_baseline(const char *name, const sh_report *now, char **why);
void sh_reports_remember(const char *cache, const char *name, const sh_source *src);  // from the cache only, never the network

#ifdef __cplusplus
}
#endif
