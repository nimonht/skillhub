# 2.0 spikes: findings

Milestone M0 of the 2.0 plan ran five short experiments before any core code was written. Each finding below changes or
confirms a design decision; the scripts stay in `tests/spikes/` so they can be run again.

## 1. Windows: standard I/O of a program without a console

Question: can the window talk to `skillhubw.exe serve --stdio` (a window-subsystem program, so no console flashes) over
pipes?

- The M0 command line already depends on the same mechanism: `skillhub-cli.exe` starts the window-subsystem `skillhub.exe`
  with its own standard handles passed through `STARTF_USESTDHANDLES`, and the Windows CI job runs the whole command-line
  test suite that way. The C runtime of a window-subsystem program picks up handles given in `STARTUPINFO`, so pipes work.
- Qt logs of such a program go to the debugger, not to stderr, unless `QT_FORCE_STDERR_LOGGING=1` is set. `tests/gui_launch.py`
  sets it.
- Still to confirm in M1c, on the Windows runner: `QProcess` starting `skillhubw.exe` (`tests/rpc_smoke.py` runs there).

## 2. Partial shallow clones and fetching only the blobs that are needed

Script: `tests/spikes/partial_clone.py [URL]`. Run against a local repository and against `github.com/anthropics/skills`
(git 2.56):

| Step | file:// | GitHub |
|---|---|---|
| `ls-remote URL HEAD` | 0.01 s | 0.87 s |
| `clone --depth 1 --filter=blob:none --no-checkout` | 0.02 s, no extra fetch | 1.05 s, no extra fetch |
| `ls-tree -r -t -z HEAD` (no sizes) | no fetch | no fetch, 96 trees and 445 blobs listed |
| `cat-file --batch` of 5 missing blobs | 5 fetches | **5 fetches, 3.27 s** |
| one `fetch --stdin` with 440 blob ids | 1 fetch | **1 fetch, 1.15 s** |
| `cat-file --batch` of those 440 blobs | local | local, 0.04 s |

Decisions:

- Never let git fetch missing blobs lazily: each one is a separate round trip (0.65 s each on GitHub). Before extracting
  changed folders, or reading `SKILL.md` front matter to find moved skills, the engine collects the blob ids from
  `ls-tree` and fetches them in one `git -c fetch.negotiationAlgorithm=noop fetch --no-tags --no-write-fetch-head
  --recurse-submodules=no --filter=blob:none --stdin origin`.
- `ls-tree -l` (with sizes) needs the blobs, so on a partial clone it is only run after the prefetch. Size caps are enforced
  while extracting.
- GitHub serves blobs by id to partial clones. Local fixtures need `uploadpack.allowFilter` and
  `uploadpack.allowAnySHA1InWant`; `bench/bench.py` sets the first, the spike both.
- `ls-remote` costs one TLS round trip per repository (0.9 s to GitHub from here). A check of 500 repositories is bound by
  that, not by CPU. So lightweight `ls-remote` processes get their own, higher limit: 32 per host. Fetches keep 8 per host
  and 16 in total, because each can use tens of MB.

## 3. Writing the user lock file at the same time as `npx skills`

From npx's source (`src/skill-lock.ts`): `writeSkillLock` reads the whole file, changes it in memory and writes it back with
`writeFile`, with no temporary file and no locking (`JSON.stringify(lock, null, 2)`, no trailing newline).
`readSkillLock` returns an empty lock on any read or parse error.

- Skill Hub writes through a temporary file and a rename, so npx can never read a half-written file from Skill Hub. That
  matters: npx would treat it as empty and write an empty lock back.
- The other direction can happen: Skill Hub may read while npx is writing. Skill Hub retries a failed parse once after
  200 ms, and refuses to write when the file still does not parse (`docs/formats.md`, section 2).
- Lost updates are possible in both directions when both tools write in the same instant, because npx overwrites the whole
  file. Skill Hub narrows its side of the window: it re-reads and merges per entry right before each write, and writes in
  group commits rather than once at the end of a long job. Nothing can prevent npx from overwriting a Skill Hub write that
  lands between npx's read and its write. The next scan then shows the skill as local, and `skillhub track` fixes it.

## 4. Qt 6.4 and the software renderer

Script: `tests/spikes/qt_fallbacks.qml`, run with the `qml` tool (`QT_FORCE_STDERR_LOGGING=1 QT_QPA_PLATFORM=offscreen
QT_QUICK_BACKEND=software qml --verbose ...`). Debian 12 (Qt 6.4.2) and Arch (Qt 6.12.0):

| Probe | Qt 6.4 | Qt 6.12 |
|---|---|---|
| `SplitView` skips a hidden child and its handle | yes | yes |
| `SplitView.saveState()` | 65 bytes, restorable | same |
| `import QtQuick.Effects` (`MultiEffect`) through a `Loader` | **error: module "QtQuick.Effects" is not installed** | loads |
| `ShaderEffect` under the software renderer | status `Uncompiled`, no error | same |
| `GraphicsInfo.api` with `QT_QPA_PLATFORM=offscreen` | software | software (offscreen has no GPU, even without `QT_QUICK_BACKEND`) |

Decisions:

- A failed import prints a QML error, and the headless tests treat every QML error as a failure. So the window never tries
  to load `QtQuick.Effects` where it is missing: C++ probes the import once, silently (a `QQmlComponent` created from
  `import QtQuick.Effects; Item {}`, errors read but not printed) and exposes the result as `app.hasEffects`. That also
  covers distributions that package the module separately.
- Materials degrade to Solid when `GraphicsInfo.api` is software (all headless tests), when `app.hasEffects` is false, or
  when transparency is reduced.
- `SplitView` behaves the same on 6.4 and 6.12, so the overlay detail panel can simply hide the docked one.

Found along the way: with stderr not a terminal, Qt on Arch sends its log to journald unless `QT_FORCE_STDERR_LOGGING=1` is
set. `tests/gui-smoke.sh` grepped stderr for QML errors and could not have seen any on such a system; it now sets the
variable (and still passes).

## 5. The project lock's `computedHash`

Script: `node tests/vectors/make-computed-hash.mjs > tests/vectors/computed-hash.json`. It reimplements npx's ten-line
`computeSkillFolderHash` with Node's own `localeCompare`, so no downloaded package runs.

- For ASCII paths the order is the same under the en-US, de-DE, sv-SE and C.UTF-8 locales. It is ICU's root collation:
  - compare the primary weights of the whole path first: space, then punctuation in the order `_ - , ; : ! ? . ' " ( ) [ ] { } @ * / \ & # % ` ^ + < = > | ~ $`, then digits, then letters ignoring case
  - only on a tie compare case, lowercase first
  - for example `a.md` < `A.md` < `b.md`, `sub-dir/a.md` < `sub.md` < `Sub/a.md` < `sub/deeper/x.md`, and `1.md` < `10.md` < `9.md`
- Non-ASCII names depend on the user's locale: Swedish sorts `ä` after `z`, while the others sort it beside `a`. So npx's
  hashes are not portable for such names even between two npx users.
- Decision: the C core implements root collation (exact for ASCII; Latin letters with accents fall back to their base
  letter plus an accent weight; `ß` sorts as `ss`; other scripts sort after Latin by code point). It is checked against the
  vectors. Hashes written by npx stay opaque (`docs/formats.md`, section 3), so a locale difference can never make Skill
  Hub replace a skill.
