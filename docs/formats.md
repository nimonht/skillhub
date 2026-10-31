# Skill Hub data formats (2.0)

This is the contract between the C core, the command line, the window and other tools that share the same files
(`npx skills` above all). A change needs a note here first. Where a rule exists for compatibility with older
versions or with `npx skills`, the reason is given.

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

The config and cache rows are exactly what Qt's `QStandardPaths` `GenericConfigLocation` / `GenericCacheLocation` return,
so the files of older versions are found. On Linux they follow `$HOME`, not `SKILLHUB_HOME`; on macOS they
follow the account's home, as `NSHomeDirectory` does. `XDG_*` values that are not absolute paths are ignored, as the XDG
specification says.

## 2. User lock file (`.skill-lock.json`, shared with `npx skills`)

### Which file

Let `npxPath` be `$XDG_STATE_HOME/skills/.skill-lock.json` when `XDG_STATE_HOME` is set and absolute, otherwise
`<home>/.agents/.skill-lock.json` (this is what `npx skills` uses). Let `legacyPath` be `<home>/.agents/.skill-lock.json`
(what older versions of Skill Hub always used).

1. With `SKILLHUB_HOME` set: `legacyPath` only (tests and portable setups).
2. `npxPath == legacyPath`: that file.
3. Only `legacyPath` exists: it is used. Nothing is switched silently.
4. Only `npxPath` exists, or neither: `npxPath` (created on the first write).
5. Both exist. This happens: npx wrote its file while an older Skill Hub kept using the other one (one real machine had
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

An entry may also hold `ref`, the branch or tag it follows, absent for the default branch. An entry from an older version, or
from npx before it wrote `sourceType`, has none and counts as `github`.

**Live links.** A `local` entry whose folder in the shared folder is a link to the source folder, not a copy, is a live
link: edits reach every agent at once. The scan accepts a link there only for an entry of type `local`, so a stray link
never turns into a skill. A live link is always current, updates skip it, and removing it removes the link alone. Its
target is never moved, changed or backed up. Everything else that walks the shared folder reads through the link as usual.

### Reading and writing

- Read the whole file. If it does not parse, read it once more after 200 ms (npx writes without locking). If it still fails,
  or `version` is greater than 3, report the problem and **do not write the file** until it is fixed. A file that could not be parsed
  is never overwritten. The same rule covers
  `providers.json` and Antigravity's `skills.json`.
- Writes merge per entry: re-read the current file, apply only our changes (added, changed and removed names), and write
  atomically: a temporary file in the same folder, `fsync`, then rename over the target. A symlinked lock file is
  resolved first, so the link survives.
- A failed write is reported to the caller.
- Jobs over many skills commit their entries in groups: every 200 entries or 5 seconds, whichever comes first, and at
  the end of the job. Each group reads and writes each file once. If the process
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

- Skill names sorted; two-space indentation; trailing newline. No timestamps: two branches that add different skills merge
  without a conflict.
- Project scope: the shared folder is `<project>/.agents/skills`. Agents whose `projectDir` is `.agents/skills` read it
  themselves, the others get relative links inside the repository, never into `~`. Backups live outside the repository, in
  `<state>/projects/<h8>/backup`. `state.json` and `sources.json` keep a project's holds and records under the key
  `project:<root>`.
- This file comes with the repository, so it is untrusted, and what an entry declares proves nothing. What counts is where
  it leads once links are followed: a folder must be inside the project, whatever `sourceType` says, and a `github` entry
  must name that repository. A link in the project's shared folder is no skill. A project whose `.agents/skills` or
  `skills-lock.json` leads outside it is refused, and an agent folder that does is left out.
- Where symlinks cannot be made, agents with a folder of their own get a copy that relink keeps identical (link state
  `synced`, healthy): a junction holds an absolute path, so it could not be committed. A copy that drifted is kept in
  `_conflicts` before it is replaced. A text file where a link belongs (git with `core.symlinks` off) is left alone, and
  `doctor` says so.
- There are no live links in a project (a link to a folder on one machine means nothing to the team) and no personal
  switches. A project's tree ids are kept in `sources.json`, as for GitLab, so a skill npx installed is offered as an update
  once.
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

- `<stamp>` is `yyyyMMdd-HHmmss` in local time, the format older versions used, so all backups sort together. A second backup in the
  same second gets `-2`, `-3`, ...
- Replacing or removing a live link leaves no backup: the folder it led to is still where it was.
- Each backup folder may have a sidecar with the same name plus `.json`:

```json
{
  "version": 1,
  "name": "<skill>",
  "scope": "user",
  "reason": "update",
  "createdAt": "2026-10-25T09:20:41.715Z",
  "createdBy": "skillhub 2.0.0",
  "treeSha": "<the upstream tree id that version came from, or empty>",
  "lockEntry": { "source": "owner/repo", "sourceType": "github", "skillFolderHash": "...", "...": "..." }
}
```

  `reason` is one of `update`, `rollback`, `remove`, `duplicate`, `conflict`, `unpark`. `scope` is `user` or
  `project:<absolute path>`. `lockEntry` is `null` for local skills. Backups from older versions have no sidecar: restoring one
  restores the files only, with an empty `skillFolderHash`, so the next check offers the update again.
- Retention: the newest N `<stamp>` folders per skill are kept (setting `updates.backupsKept`, default 3, 1–20). Pruning
  removes each folder together with its sidecar.

- `reason` is `update`, `install`, `rollback` or `remove`. A swap writes the sidecar after the new version is in place, so a
  crash at that moment leaves a backup without one, which then counts as a backup from an older version.
- Rollback backs up the current version (reason `rollback`), moves the chosen backup into place, restores its lock entry
  and tree record, and sets a hold. A backup without a sidecar brings back the files only: the lock entry stays and its hash
  is cleared, so the next check offers the update.
- Restore moves the newest `_removed/<name>-<stamp>/` back, with its lock entry. It never replaces an installed skill.
- Retention removes a backup and its sidecar together. It keeps 3 versions per skill.

## 5. Cache (`<cache>`)

| Path | What |
|---|---|
| `repos/owner__repo/` | github.com repositories without a ref (the key older versions used, kept so existing clones are reused) |
| `repos/<host>/<path>-<h8>/` | every other repository; `<path>` has `/` replaced by `__` and characters outside `[A-Za-z0-9._-]` by `_`; `<h8>` is the first 8 hex digits of SHA-1 over `<normalized url>#<ref>` |
| `repos/<key>.lock` | per-repository lock (section 8) |
| `stage/<label>-<pid>-<n>/` | staging folders: the skill name (`import-<name>` for embedded skills), the process id and a counter, so no two threads or processes share one |
| `stage/review-<name>-<pid>-<n>/` | a reviewed update: `new/` (upstream's folder) and `review.json` (`name`, `tree`, `skillPath`, and `hash`, the SHA-256 folder hash of `new/`, checked again before it is applied, so what is applied is what was shown). Kept for a day; other stages count as leftovers after ten minutes |
| `sources.json` | derived update data, safe to delete |
| `fetch-sizes.json` | folder sizes for `skillhub fetch`, remembered for an hour or until the folder changes |

- Normalized URL: lowercase scheme and host, no trailing `/` or `.git`; `git@host:path` becomes `ssh://git@host/path`.
- Clones made by older versions are full working trees, sometimes with a local path as `origin` (the tests seed one). They are used as
  they are. New clones are made with `--depth 1 --no-checkout`: everything is read from the object store (`ls-tree`,
  `cat-file --batch`), never from a working tree.
- Bringing a clone up to date: `git ls-remote origin <ref>` (`HEAD` without a ref; `<ref>` and `<ref>^{}`, so a tag is
  followed to its commit, with a tag winning over a branch of the same name) is compared with the clone's own `HEAD`.
  When they match, nothing is downloaded. Otherwise `git fetch --depth 1 origin <ref>` runs and `HEAD` is moved to
  `FETCH_HEAD^{commit}` with `git update-ref --no-deref` (the working tree of an older clone is left as it was). The clone's `HEAD` is the only record of
  what the cache holds; `sources.json` never decides whether to fetch, because a separate record could run ahead of the
  clone after a crash.
- Every git command runs with `-c core.longpaths=true -c gc.auto=0 -c maintenance.auto=false -c core.fsmonitor=false
  -c protocol.ext.allow=never -c protocol.fd.allow=never`, `GIT_TERMINAL_PROMPT=0` and `GCM_INTERACTIVE=never`. Skill Hub
  never runs garbage collection in the cache, so objects read by id stay valid while other threads and processes fetch.
- Transports: `ext::` and `fd::` can run commands, so they are never allowed, and git applies that to a URL after any
  `insteadOf` rewrite. Everything else stays as the user's git config has it, so `insteadOf` rules, SSH and private mirrors
  keep working. What decides which sources an untrusted file may name is the import check in section 9.
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

`trees` is keyed by scope (`user` or `project:<path>`). `contentHash` is the SHA-256 folder hash of what Skill Hub placed,
for every source: when the folder no longer has that hash, it holds local edits. A skill with no record (installed by
an older version or npx) has no such claim made about it. `repos`, `lastCheck` and `commit` are reserved; only `treeSha`
and `contentHash` are written so far.

`treeSha` is what makes GitLab and git checks cheap: their lock entries hold a hash of the files, and comparing that with
upstream would mean downloading them. So `check` compares the git tree id of the folder upstream with the id on record.
A skill with none on record (installed by npx, or this file was deleted) is offered as an update once, which writes it.
GitHub entries need no record because their hash in the lock is the tree id. A `local` source hashes the folder itself.

## 6. Config (`<config>`)

All files are JSON with a `version` field, written atomically. `providers.json` keeps the shape older versions wrote.

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
  "layout": { "dockAtStart": false, "bandCompact": false, "railAutoCollapse": false },
  "projects": { "scanRoots": ["~/Projects"] },
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
    "reviewBeforeUpdate": true,
    "autoSkipScripts": true,
    "checkAtLaunch": true
  },
  "projects": { "roots": [] }
}
```

- `appearance.theme`: `system`, `dark` or `light`. `accent.source`: `default` (mint), `system`, `preset` or `custom`;
  `color` is the chosen `#RRGGBB`. `material`: `solid`, `blur` or `glass`. `density`: `comfortable` or `compact`.
- `updates.parallel` and `updates.parallelPerHost` limit git fetches (each can use tens of MB);
  `updates.parallelChecksPerHost` limits the lightweight `ls-remote` checks, which are bound by network round trips.
- `terminal.shell` and `terminal.external` are empty for "detect". `importLook` is `kitty`, `ghostty`, `alacritty` or
  empty. `logo` is `builtin`, `none` or a file path. `icons` is `unicode`, `nerd` or `none`.
- Older versions kept the theme and reduce-motion choice in `QSettings`; the window moves them here on its first start.

### `toggles.json`

Per-agent switches for user-level skills. Keys are resolved agent folders (with `/`), because some agents share one:
amp and replit both read `~/.config/agents/skills`, zencoder and zenflow both read `~/.zencoder/skills`. A file that cannot
be parsed is never overwritten. Agents that read the shared folder have no entry here: where the folder sits says it
(section 7). Removing a skill forgets its switches.

```json
{
  "version": 1,
  "disabled": {
    "<skill>": ["/home/me/.claude/skills"]
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
  or `skillhub release NAME`, removes the hold. `skillhub hold NAME` sets one with an empty `fromTree`, which holds whatever
  upstream does.
- `parked`: lock entries of parked skills, kept here while parked. `npx skills update -g` reinstalls any lock entry whose
  upstream changed without checking that its folder exists, so a parked entry left in the shared lock would come back.
  Unparking returns the entry to the lock file.
- `automationEnv`: variables the agent catalog refers to (`CLAUDE_CONFIG_DIR`, `CODEX_HOME`, ...) and the `XDG_*` folders,
  recorded when automation is switched on. Scheduled runs (systemd, launchd, Task Scheduler) do not inherit the login
  environment; `--auto` and watcher runs use these values for variables that are unset.

## 7. Parked skills (`<home>/.agents/.parked/<name>`)

A skill switched off for every agent that reads the shared folder (`@shared`, one switch for all of them) is moved here, with its lock entry moved to
`state.json`. Link-mode agents that still have it switched on get a relative link to this folder. If the shared folder
gets a skill of the same name meanwhile (installed by npx, say), the shared folder wins and the parked copy is moved to
`.backup/.parked-replaced/`.

`skillhub enable --all-parked` brings every parked skill back.

## 8. Locks and crash safety (`<state>`)

| Path | What |
|---|---|
| `mutate.lock` | held by any process that changes skills, links, lock files or config (advisory: `flock`, or `LockFileEx` on a file used only for locking, never on the JSON files themselves, because Windows locks are mandatory and would block `npx`) |
| `<cache>/repos/<key>.lock` | held while a clone is made or brought up to date and its tree is listed, by checks too (two processes would otherwise race on git's own `shallow.lock`). Objects are then read by id without it (section 5) |
| `journal/<name>-<pid>-<n>.json` | intent journal of one swap that replaces a skill folder; recovery reads it |

- The command line takes `mutate.lock` for the commands that change something: `adopt`, `update`, `remove`, `track`,
  `import`, `uninstall`, `add` with `--all` or `--skill`, `doctor --fix`, `providers enable|disable|add|remove`, and
  `autoupdate|watch on|off`. When the lock is taken it prints "waiting for another Skill Hub operation…" to stderr and
  waits up to 60 s (`--wait SECONDS`, accepted anywhere after the command); then it prints "another Skill Hub operation
  is still running; try again later" and exits with code 75. The watcher's run (`adopt --notify`) exits at once, quietly
  and with code 0, when the lock is taken: the running operation rescans when it ends.
- Journal entry: `{ "version": 1, "name": "...", "dest": "...", "backup": "...", "stage": "..." }`, written before the
  installed folder is moved aside for its replacement and removed once the new folder is in place. A fresh install needs
  none: its one step is a rename. If placing the new folder fails, the old one is moved back at once.
- Recovery runs whenever the mutation lock is taken: by every changing command, by `skillhub serve` at start, and at the
  start of each of its changing jobs. For each journal entry whose `dest` is missing and whose `backup` exists, the
  backup is moved back; the entry's stage folder is removed. Paths outside the shared, backup and stage folders are never
  touched. Staging folders and git `*.lock` files in cache clones whose `<key>.lock` is free are removed once they are
  more than ten minutes old: an older Skill Hub still installed takes neither lock, and a younger one may be its.

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
  per skill. A skill over a limit is left out of the export and listed, so every export file can be imported again.
- Remote entries must use `https`. Local paths, `file:`, `ssh:` and `git@` sources are rejected on import, so a hostile
  file cannot make Skill Hub copy a folder from this machine or use its SSH keys. So are URLs with a user name or token
  in them, a `#ref` or a folder inside `source`, and a `ref` that is not a plain branch or tag. The URL in the file is rebuilt from the
  validated source. An entry without `sourceType` is GitHub-only, as version 1 files are.
- A skill whose source cannot be imported elsewhere travels as files instead (`"local": true`): local folders, live links,
  ssh sources and types managed by npx. Its source stays behind, so the file names no path of this machine.
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

The user's `providers.json` in the config folder (`overrides`, `custom`) keeps the shape older versions wrote.

## 11. Command line output that scripts and tests rely on

- `list --json`: an array of `{ "name", "source", "hash", "agents": { "<agent id>": "<state>" } }`, plus
  `"scope"`, `"sourceType"`, `"lock"` (`"current"` or `"legacy"`) and `"parked"`. Link states: `link`, `shared`, `MISSING`,
  `COPY`, `OTHER`, `off` (switched off for that agent; healthy) and `synced` (a project copy kept
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

`check NAME` checks every tracked skill and its exit code counts all of them. Kept on purpose: scripts may rely
on it.

Commands that work on many repositories (`add`, `check`, `update`, `import`) handle them in parallel:

- Progress lines (`cloning …`, `git failed for …`, `installed …`, `updated …`, `linked …`, `FAILED to import …`) come in
  the order the work finishes, which is not fixed. The status lines of `check` and `update` and the closing summary line
  keep a fixed order and place.
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
| `SKILLHUB_AUTOMATION_DRYRUN` | Write the unit, plist or task files and log the commands instead of running them. `systemd`, `launchd` or `schtasks` picks that backend on any system; any other value keeps this system's own |
| `SKILLHUB_NO_SYMLINKS` | Any value: behave like an account that may not create symlinks. On Windows junctions are used for the user's own skills. Everywhere, projects get synced copies |
| `SKILLHUB_GUI_BIN` | the window program `skillhub` starts, instead of the one next to it |
| `SKILLHUB_BACKEND` | the backend program the window starts, instead of the one next to it |
| `SKILLHUB_THEME`, `SKILLHUB_REDUCE_MOTION`, `SKILLHUB_NO_HOVER` | window test switches |
