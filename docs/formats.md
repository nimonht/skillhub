# Skill Hub data formats (2.0)

This is the contract between the C core, the command line, the window and other tools that share the same files
(`npx skills` above all). It is frozen before the 2.0 core is written; a change needs a note here first. Where a rule
exists for compatibility with 1.x or with `npx skills`, the reason is given.

Paths are written with `/` on every system. Inside the core every path uses `/`; Windows APIs get native separators and the
`\\?\` long-path prefix only at the boundary.

## 1. Directories

| Name | Linux | macOS | Windows | Override |
|---|---|---|---|---|
| home | `$SKILLHUB_HOME`, else `$HOME` | `$SKILLHUB_HOME`, else the account's home (`getpwuid`) | `%SKILLHUB_HOME%`, else the profile folder of the process token (`GetUserProfileDirectoryW`; the `USERPROFILE` variable is ignored, as Qt does) | `SKILLHUB_HOME` (an empty value counts as unset) |
| shared folder | `<home>/.agents/skills` | same | same | none |
| backups | `<home>/.agents/.backup` | same | same | none |
| parked skills | `<home>/.agents/.parked` | same | same | none |
| config | `${XDG_CONFIG_HOME:-$HOME/.config}/skillhub` | `~/Library/Preferences/skillhub` | `%LOCALAPPDATA%/skillhub` | `SKILLHUB_CONFIG` is the config folder itself (set counts, even when empty) |
| cache | `${XDG_CACHE_HOME:-$HOME/.cache}/skillhub` | `~/Library/Caches/skillhub` | `%LOCALAPPDATA%/cache/skillhub` | `SKILLHUB_CACHE` is the cache folder itself (set counts, even when empty) |
| state | `${XDG_STATE_HOME:-$HOME/.local/state}/skillhub` | `~/Library/Application Support/skillhub` | `%LOCALAPPDATA%/skillhub/state` | `SKILLHUB_STATE` |

The config and cache rows are exactly what Qt's `QStandardPaths` `GenericConfigLocation` / `GenericCacheLocation` return
in 1.x, so 2.0 finds the files 1.x wrote. On Linux they follow `$HOME`, not `SKILLHUB_HOME`, as in 1.x; on macOS they
follow the account's home, as `NSHomeDirectory` does. `XDG_*` values that are not absolute paths are ignored, as the XDG
specification says.

## 2. User lock file (`.skill-lock.json`, shared with `npx skills`)

### Which file

Let `npxPath` be `$XDG_STATE_HOME/skills/.skill-lock.json` when `XDG_STATE_HOME` is set and absolute, otherwise
`<home>/.agents/.skill-lock.json` (this is what `npx skills` uses). Let `legacyPath` be `<home>/.agents/.skill-lock.json`
(what Skill Hub 1.x always used).

1. With `SKILLHUB_HOME` set: `legacyPath` only (tests and portable setups).
2. `npxPath == legacyPath`: that file.
3. Only `legacyPath` exists: it is used. Nothing is switched silently.
4. Only `npxPath` exists, or neither: `npxPath` (created on the first write).
5. Both exist. This happens: npx wrote its file while Skill Hub 1.x kept using the other one (the author's machine had
   1 entry in one and 128 in the other). Both are read:
   - An entry in both files counts with the newer `updatedAt`.
   - A change to an entry goes to the file or files that already hold it; a new entry goes to `npxPath`; a removed skill
     is removed from both.
   - Nothing is copied from one file to the other, so an entry that `npx skills remove` deleted from its file never comes
     back there.
   - `doctor` reports the split and counts it as a problem, `list --json` marks entries held only by `legacyPath` with
     `"lock": "legacy"`, and the window shows a "Merge" banner.

`skillhub doctor --fix`, or the banner, merges once: it backs up `npxPath` (`<file>.bak-<stamp>`), copies into it the
legacy entries whose folder exists in the shared folder, moves `legacyPath` aside to `<file>.bak-<stamp>`, and records the
merge in `state.json` (section 6). From then on only `npxPath` exists.

### Shape

```json
{
  "version": 3,
  "skills": {
    "<name>": {
      "source": "owner/repo",
      "sourceType": "github",
      "sourceUrl": "https://github.com/owner/repo.git",
      "ref": "main",
      "skillPath": "skills/<name>/SKILL.md",
      "skillFolderHash": "<40 hex>",
      "installedAt": "2026-10-25T09:20:41.715Z",
      "updatedAt": "2026-10-25T09:20:41.715Z"
    }
  },
  "dismissed": { "findSkillsPrompt": true },
  "lastSelectedAgents": ["claude-code"]
}
```

- `version` stays 3. `npx skills` throws away files with a lower version, so the core never writes one.
- Optional fields: `ref`, `skillPath`, plus `pluginName`, `sourceBaseUrl` and `wellKnownDigest`, which npx writes for some
  sources. Unknown fields, top-level or per entry, are kept as they are: the core edits a copy of the parsed document and
  touches only the entries and fields it owns.
- Timestamps are ISO 8601 in UTC with milliseconds and `Z`, as JavaScript's `toISOString()` writes them.
- Written exactly as npx writes it, so a dotfiles repository shows no churn: two-space indentation, no trailing newline.
- npx itself writes this file in place (no temporary file, no locking) after reading the whole file, and treats a file it
  cannot read as empty. Skill Hub's atomic rename means npx never reads a half-written file from Skill Hub; the opposite can
  happen, which is why reads retry (below).

### Entries by source type

These follow `npx skills` `src/add.ts`, so both tools understand each other's entries.

| `sourceType` | `source` | `sourceUrl` | `skillFolderHash` |
|---|---|---|---|
| `github` | `owner/repo` | `https://github.com/owner/repo.git` | git tree SHA-1 of the skill folder (40 hex) |
| `gitlab` | the repository URL | the repository URL | SHA-256 folder hash (section 3), 64 hex |
| `git` | the repository URL | the repository URL | SHA-256 folder hash |
| `local` | the folder's absolute path | the same | SHA-256 folder hash |
| anything else (`well-known`, `download`, `mintlify`, ...) | as npx wrote it | | as npx wrote it |

Entries of a type Skill Hub does not run are listed as "managed by npx": never fetched, never updated, never rewritten.

### Reading and writing

- Read the whole file. If it does not parse, read it once more after 200 ms (npx writes without locking). If it still fails,
  or `version` is greater than 3, report the problem and **do not write the file** until it is fixed. 1.x turned a broken
  file into `{}` and wrote that back; 2.0 never overwrites a file it could not parse. The same rule covers
  `providers.json` and Antigravity's `skills.json`.
- Writes merge per entry: re-read the current file, apply only our changes (added, changed and removed names), and write
  atomically: a temporary file in the same folder, `fsync`, then rename over the target. A symlinked lock file is
  resolved first, so the link survives.
- A failed write is reported to the caller (1.x ignored it).
- Jobs over many skills commit their entries in groups: every 200 entries or 5 seconds, whichever comes first, and at
  the end of the job. Each group reads and writes each file once (1.x rewrote the file after every skill). If the process
  is killed between two commits, the folders swapped since the last one have no lock entry yet and show as local skills
  until they are tracked again. The swap journal (section 8) closes that gap.

## 3. Project lock file (`<project>/skills-lock.json`)

Written exactly like `npx skills` `src/local-lock.ts`, because it is committed and shared:

```json
{
  "version": 1,
  "skills": {
    "<name>": {
      "source": "owner/repo",
      "sourceType": "github",
      "sourceUrl": "https://github.com/owner/repo.git",
      "skillPath": "skills/<name>/SKILL.md",
      "computedHash": "<64 hex>"
    }
  }
}
```

- Skill names sorted; two-space indentation; trailing newline.
- Optional fields: `sourceUrl`, `ref`, `skillPath`, `version`, `via`, `subagents`, `wellKnownDigest`. Unknown fields are
  kept.
- `local` sources are stored relative to the project: `./path`, or `../path` when outside it; a path on another Windows
  drive stays absolute with `/`.
- **`computedHash`** (the "SHA-256 folder hash" used throughout this document): SHA-256 over every regular file below the
  skill folder, skipping folders named `.git` and `node_modules` and anything that is not a regular file (links
  included). For each file in order, feed the relative path with `/` separators, then the file's bytes, with no separator.
  Files are ordered by relative path the way JavaScript's `localeCompare` does with no locale given. The core implements
  an ICU-compatible ordering for ASCII paths and is checked against vectors produced by real `npx skills`
  (`tests/vectors/computed-hash.json`).
- Hashes that npx wrote are opaque to Skill Hub: change detection compares against the hash Skill Hub itself last
  computed (cache `sources.json`), never against a foreign one.

## 4. Backups (`<home>/.agents/.backup`)

| Path | Written by |
|---|---|
| `<name>/<stamp>/` | update, rollback (the version that was replaced) |
| `_removed/<name>-<stamp>/` | remove |
| `_dupes/<agent>-<name>-<stamp>/` | adopt (identical copy in an agent folder) |
| `_conflicts/<agent>-<name>-<stamp>/` | adopt (a different copy in an agent folder) |
| `.parked-replaced/<name>-<stamp>/` | unpark when the shared folder already holds a skill of that name |

- `<stamp>` is `yyyyMMdd-HHmmss` in local time, as in 1.x, so old and new backups sort together. A second backup in the
  same second gets `-2`, `-3`, ...
- Each backup folder may have a sidecar with the same name plus `.json`:

```json
{
  "version": 1,
  "name": "<skill>",
  "scope": "user",
  "reason": "update",
  "createdAt": "2026-10-25T09:20:41.715Z",
  "createdBy": "skillhub 2.0.0",
  "treeSha": "<40 hex: git tree SHA-1 of the backed-up folder>",
  "lockEntry": { "source": "owner/repo", "sourceType": "github", "skillFolderHash": "...", "...": "..." }
}
```

  `reason` is one of `update`, `rollback`, `remove`, `duplicate`, `conflict`, `unpark`. `scope` is `user` or
  `project:<absolute path>`. `lockEntry` is `null` for local skills. Backups from 1.x have no sidecar: restoring one
  restores the files only, with an empty `skillFolderHash`, so the next check offers the update again.
- Retention: the newest N `<stamp>` folders per skill are kept (setting `updates.backupsKept`, default 3, 1–20). Pruning
  removes each folder together with its sidecar.

## 5. Cache (`<cache>`)

| Path | What |
|---|---|
| `repos/owner__repo/` | github.com repositories without a ref (the 1.x key, kept so existing clones are reused) |
| `repos/<host>/<path>-<h8>/` | every other repository; `<path>` has `/` replaced by `__` and characters outside `[A-Za-z0-9._-]` by `_`; `<h8>` is the first 8 hex digits of SHA-1 over `<normalized url>#<ref>` |
| `repos/<key>.lock` | per-repository lock (section 8) |
| `stage/<label>-<pid>-<n>/` | staging folders: the skill name (`import-<name>` for embedded skills), the process id and a counter, so no two threads or processes share one (1.x named them after the skill, so two processes could delete each other's) |
| `sources.json` | derived update data, safe to delete |

- Normalized URL: lowercase scheme and host, no trailing `/` or `.git`; `git@host:path` becomes `ssh://git@host/path`.
- Clones made by 1.x are full working trees, sometimes with a local path as `origin` (the tests seed one). They are used as
  they are. New clones are made with `--depth 1 --no-checkout`: everything is read from the object store (`ls-tree`,
  `cat-file --batch`), never from a working tree.
- Bringing a clone up to date: `git ls-remote origin HEAD` is compared with the clone's own `HEAD`. When they match,
  nothing is downloaded. Otherwise `git fetch --depth 1 origin HEAD` runs and `HEAD` is moved to `FETCH_HEAD` with
  `git update-ref --no-deref` (a 1.x clone's working tree is left as it was). The clone's `HEAD` is the only record of
  what the cache holds; `sources.json` never decides whether to fetch, because a separate record could run ahead of the
  clone after a crash.
- Every git command runs with `-c core.longpaths=true -c gc.auto=0 -c maintenance.auto=false -c core.fsmonitor=false`,
  `GIT_TERMINAL_PROMPT=0` and `GCM_INTERACTIVE=never`. Skill Hub never runs garbage collection in the cache, so objects
  read by id stay valid while other threads and processes fetch.
- Staging folders are created on the same volume as the shared folder when the cache is on a different one, so the
  final move is a rename.

`sources.json`:

```json
{
  "version": 1,
  "repos": { "<cache key>": { "url": "...", "ref": "", "head": "<commit>", "checkedAt": "<ISO UTC>" } },
  "trees": { "user": { "<skill>": { "treeSha": "<40 hex>", "contentHash": "<64 hex>", "commit": "<commit>" } } },
  "lastCheck": { "user": { "at": "<ISO UTC>", "available": 3, "errors": 0 } }
}
```

`trees` is keyed by scope (`user` or `project:<path>`). `contentHash` is the SHA-256 folder hash Skill Hub last computed,
for local, gitlab and git sources and for project locks.

## 6. Config (`<config>`)

All files are JSON with a `version` field, written atomically. `providers.json` keeps its 1.x shape.

### `settings.json`

Everything that both the command line and the window need. Missing keys mean the default.

```json
{
  "version": 1,
  "appearance": {
    "theme": "system",
    "accent": { "source": "default", "color": "#34E0A6" },
    "tintSurfaces": false,
    "material": "solid",
    "intensity": 0.8,
    "reduceMotion": false,
    "reduceTransparency": false,
    "uiScale": 1.0,
    "density": "comfortable"
  },
  "terminal": {
    "shell": "",
    "external": "",
    "externalTemplate": "",
    "openExternally": false,
    "fontFamily": "",
    "fontSize": 0,
    "importLook": "",
    "cursor": "block",
    "scrollback": 10000,
    "banner": true,
    "logo": "builtin",
    "icons": "unicode"
  },
  "updates": {
    "parallel": 16,
    "parallelPerHost": 8,
    "parallelChecksPerHost": 32,
    "backupsKept": 3,
    "reviewBeforeUpdate": false,
    "autoSkipScripts": true,
    "checkAtLaunch": true
  },
  "projects": { "roots": [] }
}
```

- `appearance.theme`: `system`, `dark` or `light`. `accent.source`: `default` (mint), `system`, `preset` or `custom`;
  `color` is the chosen `#RRGGBB`. `material`: `solid`, `blur` or `glass`. `density`: `comfortable` or `compact`.
- `updates.parallel` and `updates.parallelPerHost` limit git fetches (each can use tens of MB);
  `updates.parallelChecksPerHost` limits the lightweight `ls-remote` checks, which are bound by network round trips
  (`docs/spikes.md`, spike 2).
- `terminal.shell` and `terminal.external` are empty for "detect". `importLook` is `kitty`, `ghostty`, `alacritty` or
  empty. `logo` is `builtin`, `none` or a file path. `icons` is `unicode`, `nerd` or `none`.
- 1.x kept the theme and reduce-motion choice in `QSettings`; the window moves them here on its first 2.0 start.

### `toggles.json`

Per-agent switches for user-level skills. Keys are resolved agent folders (with `/`), because some agents share one:
amp and replit both read `~/.config/agents/skills`, zencoder and zenflow both read `~/.zencoder/skills`. The key `@shared`
stands for every agent that reads the shared folder itself (and config-mode agents such as Antigravity).

```json
{
  "version": 1,
  "disabled": {
    "<skill>": ["/home/me/.claude/skills", "@shared"]
  }
}
```

Project skills have no per-user switches in 2.0: they are committed and shared with a team.

### `projects.json`

```json
{ "version": 1, "projects": [ { "path": "/home/me/Projects/app", "name": "app", "addedAt": "<ISO UTC>", "autoUpdate": false } ] }
```

### `state.json`

Durable state that Skill Hub owns and that is not a preference.

```json
{
  "version": 1,
  "holds": { "user": { "<skill>": { "fromTree": "<40 hex>", "since": "<ISO UTC>" } } },
  "parked": { "<skill>": { "lockEntry": { "...": "..." }, "parkedAt": "<ISO UTC>" } },
  "lockMigration": { "from": "<legacyPath>", "to": "<npxPath>", "at": "<ISO UTC>", "entries": 12 },
  "automationEnv": { "CLAUDE_CONFIG_DIR": "/home/me/.claude", "CODEX_HOME": "/home/me/.codex" }
}
```

- `holds`: after a rollback, automatic updates skip the skill while upstream's tree is still `fromTree`. A manual update,
  or `skillhub release NAME`, removes the hold.
- `parked`: lock entries of parked skills, kept here while parked. `npx skills update -g` reinstalls any lock entry whose
  upstream changed without checking that its folder exists, so a parked entry left in the shared lock would come back.
  Unparking returns the entry to the lock file.
- `automationEnv`: variables the agent catalog refers to (`CLAUDE_CONFIG_DIR`, `CODEX_HOME`, `XDG_CONFIG_HOME`, ...),
  recorded when automation is switched on. Scheduled runs (systemd, launchd, Task Scheduler) do not inherit the login
  environment; `--auto` and watcher runs use these values for variables that are unset.

## 7. Parked skills (`<home>/.agents/.parked/<name>`)

A skill switched off for every agent that reads the shared folder is moved here, with its lock entry moved to
`state.json`. Link-mode agents that still have it switched on get a relative link to this folder. If the shared folder
gets a skill of the same name meanwhile (installed by npx, say), the shared folder wins and the parked copy is moved to
`.backup/.parked-replaced/`.

Skill Hub 1.x knows nothing about parked skills: run `skillhub enable --all-parked` before downgrading.

## 8. Locks and crash safety (`<state>`)

| Path | What |
|---|---|
| `mutate.lock` | held by any process that changes skills, links, lock files or config (advisory: `flock`, or `LockFileEx` on a file used only for locking, never on the JSON files themselves, because Windows locks are mandatory and would block `npx`) |
| `<cache>/repos/<key>.lock` | held while a clone is made or brought up to date and its tree is listed, by checks too (two processes would otherwise race on git's own `shallow.lock`). Objects are then read by id without it (section 5) |
| `journal/<job>.json` | intent journal written before each swap; recovery reads it at start |

- The command line takes `mutate.lock` for the commands that change something: `adopt`, `update`, `remove`, `track`,
  `import`, `uninstall`, `add` with `--all` or `--skill`, `doctor --fix`, `providers enable|disable|add|remove`, and
  `autoupdate|watch on|off`. When the lock is taken it prints "waiting for another Skill Hub operation…" to stderr and
  waits up to 60 s (`--wait SECONDS`, accepted anywhere after the command); then it prints "another Skill Hub operation
  is still running; try again later" and exits with code 75. The watcher's run (`adopt --notify`) exits at once, quietly
  and with code 0, when the lock is taken: the running operation rescans when it ends.
- Journal entry: `{ "version": 1, "job": "...", "steps": [ { "op": "swap", "name": "...", "stage": "...",
  "backup": "...", "dest": "...", "state": "planned|backed-up|placed" } ] }`. On start, a swap left at `backed-up` is
  rolled back from its backup; one left at `planned` has its stage folder removed. Stale stage folders and git `*.lock`
  files in repositories whose `<key>.lock` is free are cleaned up.

## 9. Export file (`skillhub-export`)

Version 2 is written; versions 1 and 2 are read.

```json
{
  "format": "skillhub-export",
  "version": 2,
  "createdAt": "<ISO UTC>",
  "createdBy": "skillhub 2.0.0",
  "skills": [
    { "name": "a", "description": "...", "sourceType": "github", "source": "owner/repo", "url": "https://github.com/owner/repo.git", "path": "skills/a/SKILL.md", "ref": "", "hash": "..." },
    { "name": "b", "description": "...", "sourceType": "gitlab", "source": "https://gitlab.com/group/sub/repo.git", "url": "https://gitlab.com/group/sub/repo.git", "path": "SKILL.md" },
    { "name": "c", "description": "...", "local": true, "files": [ { "path": "SKILL.md", "exec": false, "data": "<base64>" } ] }
  ]
}
```

- Limits, the same on export and import: 600 skills, 48 MB file, 12 MB per local skill, 2 MB per embedded file, 800 files
  per skill. 1.x checked them only on import, so a large library could export a file it could not import again. A skill
  over a limit is left out of the export and listed.
- Remote entries must use `https`. Local paths, `file:`, `ssh:` and `git@` sources are rejected on import, so a hostile
  file cannot make Skill Hub copy a folder from this machine or use its SSH keys. The URL in the file is rebuilt from the
  validated source, as in 1.x. Version 1 entries are GitHub-only, as before.
- Names: `^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$` without `..`. File paths: relative, `/`-separated, at most 300 characters, no
  empty, `.` or `..` segments, no backslash, no NUL, no segment named `.git` in any letter case, no Windows reserved name
  (`CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9`, with or without an extension), no `:`, no segment ending in
  `.` or a space, and no two paths in one skill that differ only in letter case.

## 10. Agent catalog (`providers.json`, embedded)

Version 2 adds `projectDir` to each agent: the folder, relative to a project root, where the agent reads project skills
(from `skillsDir` in the skills CLI's `src/agents.ts`). Agents with `projectDir` `.agents/skills` read a project's shared
folder natively.

```json
{ "format": "skillhub-providers", "version": 2, "source": "vercel-labs/skills src/agents.ts @ <commit> (MIT) with skillhub overrides",
  "providers": [ { "id": "claude-code", "label": "Claude Code", "mode": "link", "dir": "${CLAUDE_CONFIG_DIR:-~/.claude}/skills",
                   "projectDir": ".claude/skills", "detect": ["${CLAUDE_CONFIG_DIR:-~/.claude}"], "bins": ["claude"],
                   "extraDirs": [], "ignore": ["synced"] } ] }
```

The user's `providers.json` in the config folder (`overrides`, `custom`) keeps its 1.x shape.

## 11. Command line output that scripts and tests rely on

- `list --json`: an array of `{ "name", "source", "hash", "agents": { "<agent id>": "<state>" } }` as in 1.x, plus
  `"scope"`, `"sourceType"`, `"lock"` (`"current"` or `"legacy"`) and `"parked"`. Link states: `link`, `shared`, `MISSING`,
  `COPY` and `OTHER` as in 1.x, plus `off` (switched off for that agent; healthy) and `synced` (a project copy kept
  identical where links cannot be made; healthy).
- Messages that tests match stay word for word, for example `0 change(s)`, `healthy`, `problems: N`,
  `nothing to install`, `aborted`, `N update(s) available`.

Exit codes:

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | the operation failed, or `doctor` found problems |
| 2 | usage error |
| 10 | `check`: updates are available |
| 75 | another Skill Hub operation holds the lock |
| 127 | the window (`skillhub-gui`) or the backend could not be started |

`check NAME` checks every tracked skill and its exit code counts all of them, as in 1.x. Kept on purpose: scripts may rely
on it.

Commands that work on many repositories (`add`, `check`, `update`, `import`) handle them in parallel:

- Progress lines (`cloning …`, `git failed for …`, `installed …`, `updated …`, `linked …`, `FAILED to import …`) come in
  the order the work finishes, which is not fixed. The status lines of `check` and `update` and the closing summary line
  keep their 1.x order and place.
- A repository that cannot be reached is reported once (`git failed for owner/repo: …`), not once per skill; each of its
  skills still shows the error in its own status line.
- `add` with several repositories reads all of them first, then installs. When two of them offer a skill of the same
  name, the repository named last wins, as if they had been added one after the other, and the line
  `skipping NAME from OWNER/REPO: OTHER/REPO has a skill of that name too` says so.

## 12. Environment variables

| Variable | Effect |
|---|---|
| `SKILLHUB_HOME` | home folder for skills, links and backups; an empty value counts as unset |
| `SKILLHUB_CONFIG` | the config folder itself; being set counts, even when empty |
| `SKILLHUB_CACHE` | the cache folder itself; being set counts, even when empty |
| `SKILLHUB_STATE` | the state folder itself |
| `SKILLHUB_BIN_PATH` | replaces `PATH` when looking for agents' commands (tests) |
| `SKILLHUB_NO_SYMLINKS` | Windows only, any value: behave like an account that may not create symlinks (junctions are used) |
| `SKILLHUB_GUI_BIN` | the window program `skillhub` starts, instead of the one next to it |
| `SKILLHUB_BACKEND` | the backend program the window starts, instead of the one next to it |
| `SKILLHUB_AUTOMATION_DRYRUN` | log the commands and files automation would run or write, instead of doing it (tests) |
| `SKILLHUB_THEME`, `SKILLHUB_REDUCE_MOTION`, `SKILLHUB_NO_HOVER` | window test switches, as in 1.x |
