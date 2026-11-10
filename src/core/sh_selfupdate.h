// Skill Hub updating itself (skills are sh_engine's business). Which versions exist comes from `git ls-remote` on the project's
// repository, the same way everything else reaches the network. The files come from the GitHub release through the system's
// curl or wget, so no HTTP or TLS library is linked in.
//
// Nothing is installed unless the release's SHA256SUMS carries a signature by the project's key and the file matches it. The
// programs are not code-signed and a download made here carries no quarantine flag, so this check is the only thing between a
// taken-over account and every machine. A release without a good signature is treated as if it did not exist.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sh_str.h"
#include "yyjson.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SH_INSTALL_SOURCE,    // built by hand: pull and rebuild
    SH_INSTALL_PACKAGE,   // Linux, below /usr: a package, which nothing here may touch
    SH_INSTALL_ZIP,       // Windows, the portable zip: a notice only, a running folder cannot replace itself safely
    SH_INSTALL_SETUP,     // Windows, the installer's folder
    SH_INSTALL_APP,       // macOS, the app
    SH_INSTALL_PORTABLE,  // Linux, the portable tarball
} sh_install_kind;

// What this program was built as. Each program defines sh_update_build_info (src/cli/update_build.c), so the test program can
// trust a test key and no shipped program ever does.
typedef struct {
    const char *version;      // this program's
    const char *public_key;   // 64 hex digits; empty: this build installs nothing
    bool release_build;       // made by the release workflow, so it can replace itself
} sh_update_build;
void sh_update_build_info(sh_update_build *b);

typedef struct {
    sh_install_kind kind;
    bool available;     // there is a version to move to: a newer one, or see `downgrade`
    bool can_install;   // and this program can install it by itself
    char *current;      // this version
    char *latest;       // that version, "" when there is none
    char *page;         // the release page of `latest`, "" when there is none
    char *advice;       // what to do when it cannot install it, "" otherwise
    char *note;         // why a newer release was passed over (no good signature), "" otherwise
    const char *channel;  // "stable" or "beta": what this install follows
    bool downgrade;     // `latest` is older than this version: the newest final one, for a pre-release that was told to follow stable
} sh_update_info;
void sh_update_info_free(sh_update_info *u);
void sh_update_info_json(yyjson_mut_doc *d, yyjson_mut_val *o, const sh_update_info *u);

// Versions are 2.0.0, or 2.0.0-beta2 with a lowercase label and an optional number. A label sorts before the final version,
// labels sort by name (beta before rc), and numbers as numbers (beta10 after beta2).
bool sh_version_valid(const char *v);
int sh_version_compare(const char *a, const char *b);  // <0, 0 or >0; both must be valid

// Which versions an install is offered. Stable: final versions only. Beta: pre-releases too.
typedef enum { SH_CHANNEL_STABLE, SH_CHANNEL_BETA } sh_update_channel;
const char *sh_update_channel_name(sh_update_channel channel);
// `chosen` is "stable" or "beta"; anything else (NULL, "") means nobody chose, and the install follows what it is: a pre-release
// follows beta, a final version stable. That is what every version before 2.0.0 does, with no choice.
sh_update_channel sh_update_channel_of(const char *running, const char *chosen);

// The oldest version a pre-release goes back to: the first one that can switch channels. Anything older would be a dead end.
#define SH_UPDATE_FLOOR "2.0.0"

// The versions in `tags` (v2.0.0 or refs/tags/v2.0.0) that `running` may move to on `channel`, newest first, at most `max`.
// Beta: every newer version. Stable: every newer final version; and when there is none and `running` is a pre-release, the
// final versions below it, down to SH_UPDATE_FLOOR, which is the one way anything moves to an older version. A -dev build
// follows nothing.
void sh_update_candidates(const sh_strv *tags, const char *running, sh_update_channel channel, size_t max, sh_strv *out);

// `signature` is 64 raw bytes by the key `public_key` (64 hex digits) over `sums`.
bool sh_update_signature_ok(const void *sums, size_t sums_len, const void *signature, size_t signature_len, const char *public_key);
// The lowercase hex SHA-256 listed for exactly `name` in SHA256SUMS text. `out` holds 65 bytes.
bool sh_update_sum_of(const char *sums, const char *name, char out[65]);

const char *sh_update_kind_name(sh_install_kind kind);
sh_install_kind sh_update_detect(const sh_update_build *b, const char *exe);  // `exe`: the path of the running program

// Looks for a version to move to. False only when the repository cannot be reached; `err` then says why. `cache`: the cache
// folder. `chosen`: the channel as for sh_update_channel_of, NULL for what settings.json says (updates.channel).
bool sh_update_check(const sh_update_build *b, const char *cache, const char *chosen, sh_update_info *out, char **err);

typedef void (*sh_update_log)(void *user, const char *line);
// Downloads and verifies `u->latest` and puts it in place. `then` receives the program to start once this one has stopped,
// with its arguments, or stays empty: on Windows that is the installer, which does the replacing, and elsewhere the new
// version when `relaunch` is set.
bool sh_update_install(const sh_update_build *b, const sh_update_info *u, const char *cache, bool relaunch, sh_update_log log, void *user,
                       sh_strv *then, char **err);

// What an earlier update left behind: downloads, and on macOS the replaced app. Call it when no update is running.
void sh_update_tidy(const char *cache);

#ifdef __cplusplus
}
#endif
