# Skill Hub backend protocol (version 1)

The window (`skillhub-gui`) does no file, git or link work itself. It starts the C backend as a child process and talks
to it over the child's standard input and output:

```
skillhub serve --stdio          # Linux, macOS
skillhubw.exe serve --stdio     # Windows (no console window)
```

The backend found next to the window is used, unless `SKILLHUB_BACKEND` names another one. The same messages can be sent
by tests (`tests/rpc_smoke.py`) or by any other front end. Data shapes (lock entries, settings, link states) are the ones
in `docs/formats.md`.

## Transport

- One JSON-RPC 2.0 message per line: UTF-8 JSON with no raw newlines, terminated by `\n`. At most 64 MB per message.
  Windows pipes run in binary mode.
- Standard error carries diagnostics only and is never parsed.
- The backend writes protocol messages to a private copy of its original standard output and points file descriptor 1
  at standard error, so a stray `printf` or a child process that inherits stdout can never corrupt the stream. Every child
  process (git, `systemctl`, ...) gets its output through its own pipe.
- Requests have an `id` (integer or string) and get exactly one response. Notifications have no `id`. The backend sends
  responses and notifications in any order. Batches (a JSON array of messages) are not supported: they get error
  `-32600`. Blank lines are ignored.
- The window parses on a worker thread and hands finished objects to the UI thread.

## Lifetime

1. The window starts the backend and sends `initialize`. No other request is accepted before it.
2. To quit, the window sends `shutdown`. The backend stops jobs at their next safe point, commits what finished, replies,
   and exits when its standard input closes. The window waits up to 10 seconds. Only after that does it kill the process:
   destroying a `QProcess` kills its child at once, which must not happen in the middle of a swap.
3. If the backend exits unexpectedly, the window starts a new one (after 0.5 s, 1 s, 2 s, then every 5 s), sends
   `initialize` and takes a fresh snapshot. The window shows "The background service restarted". The new backend's
   recovery (`docs/formats.md`, section 8) finishes or rolls back an interrupted swap.

### `initialize`

```json
→ {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocol":1,"client":"skillhub-gui 2.0.0"}}
← {"jsonrpc":"2.0","id":1,"result":{"protocol":1,"version":"2.0.0","pid":4242,
     "capabilities":{"automation":"systemd","links":"symlink","projects":true,"terminalFetch":true},
     "paths":{"home":"...","central":"...","backup":"...","cache":"...","state":"...","config":"...","lock":"..."}}}
```

When the protocol numbers differ, the backend answers with error `-32002` and the window says "backend and window are
from different versions". `capabilities.automation` is `systemd`, `launchd`, `taskscheduler` or `none`;
`capabilities.links` is `symlink` or `junction`.

### `shutdown`

`→ {"id":9,"method":"shutdown"}`, `← {"id":9,"result":{"cancelled":["<job>"]}}`

## State: snapshot and deltas

The backend keeps the current state of the selected scope and numbers every change with a revision (`rev`), assigned on
its event-loop thread.

### `state.snapshot`

```json
→ {"id":2,"method":"state.snapshot","params":{"page":0,"pageSize":1000,"rescan":false}}
← {"id":2,"result":{"rev":17,"scope":"user","page":0,"pages":1,
     "skills":[ <Skill>, ... ],
     "agents":[ <AgentTile>, ... ], "overflow":{ "count":3, "problems":0, "health":"ok", "names":"..." },
     "providers":[ <Provider>, ... ], "projects":[ <Project>, ... ],
     "counts":{ "skills":137, "updates":3, "local":9, "problems":0, "strays":0, "checked":128, "wiringOk":true }}}
```

Only page 0 carries `agents`, `providers`, `projects` and `counts`. Later pages carry `skills` only. Every page carries
`rev`; when a later page has a different `rev` than page 0, a delta came in between and the window starts over. The
window appends rows page by page, so startup stays responsive with 10,000 skills. With `"rescan":true` the backend reads
the shared folder again before it answers (the window does this when it regains focus); a snapshot asked for before the
first scan has finished is answered when it has.

`<Skill>`:

```json
{ "name":"gsap-core", "description":"...", "source":"greensock/gsap-skills", "sourceType":"github",
  "status":"available", "note":"", "links":["link","shared","off"], "problem":false,
  "parked":false, "localEdits":false, "hold":false, "versions":2, "lock":"current",
  "path":"~/.agents/skills/gsap-core", "upstreamPath":"skills/gsap-core/SKILL.md", "hash":"<tree id>",
  "installedAt":"<ISO UTC>", "updatedAt":"<ISO UTC>" }
```

`status` is `local`, `tracked`, `current`, `available`, `updated` or `error`, as 1.x shows them. A check result stays
attached to the lock entry it was made for: when the entry later holds the hash upstream had (another process updated
the skill), the status becomes `current`; when it changes to anything else, the result is dropped and the status is
`tracked` again. `links` has one link state per entry of `agents`, in that order (the overflow agents follow, in the
order of `overflow.names`): `link`, `shared`, `missing`, `copy` or `foreign`, and later `off` and `synced`. `problem` is
true for `error` and for `missing`, `copy` or `foreign` links. `versions` counts the backups kept for the skill.

`<AgentTile>` holds what the agent column shows: `id`, `label`, `short`, `monogram`, `mode`, `mechanism`, `folder`,
`detected`, `detectedBy`, `wired`, `ok`, `problems`, `total`, `health`. Marks (icons) stay in the window. `<Provider>` is
the same plus `managed` and `custom`, for the agents manager.

### `state.delta` (notification)

```json
← {"method":"state.delta","params":{"baseRev":17,"rev":18,
     "upserts":[ <Skill>, ... ], "removes":["old-skill"],
     "counts":{ ... }, "agents":[ ... ], "overflow":{ ... }}}
```

- `counts`, `agents`, `overflow` and `providers` are absolute values, sent whenever they changed; the window never adds
  them up.
- Deltas are merged for up to 50 ms before they are sent. A delta is never dropped.
- If `baseRev` is not the window's current `rev`, the window asks for a fresh snapshot.
- When the backend cannot keep up (its write queue for this client is over 8 MB), it drops pending deltas and sends
  `state.resync` instead. The window then takes a fresh snapshot.

### `scope.set`

`{"method":"scope.set","params":{"scope":"project:/home/me/Projects/app"}}` switches the state the backend reports;
the result is a snapshot like `state.snapshot` page 0. `"scope":"user"` switches back.

## Jobs

Anything that changes files or talks to the network is a job. The request returns at once:

`← {"id":5,"result":{"job":"j7"}}`

Then come notifications:

```json
← {"method":"event.progress","params":{"job":"j7","done":120,"total":10000,"current":"gsap-core"}}
← {"method":"event.log","params":{"lines":[{"time":"18:41:07","level":"info","text":"updated gsap-core -> 1a2b3c4d"}]}}
← {"method":"event.jobDone","params":{"job":"j7","kind":"update","ok":true,
     "summary":{"updated":118,"failed":2,"skipped":0},
     "results":[{"name":"gsap-core","ok":true},{"name":"x","ok":false,"error":"not found upstream"}]}}
```

- Progress is sent at most every 50 ms per job; log lines are batched. When the write queue fills, waiting progress and
  log events are merged (the newest progress, the log lines concatenated). They are never dropped.
- The window composes toasts from `event.jobDone`, as 1.x composed them from its own job results.
- `jobs.cancel {"job":"j7"}` stops a job at its next safe point: between skills, never inside a swap. Finished items stay
  done and are committed; staging folders of unfinished ones are removed. The job then ends with `"ok":false,
  "cancelled":true`.
- One job runs at a time. Another job request while one runs gets error `1001` (busy). Requests that are not jobs
  (`state.snapshot`, `skills.read`, `settings.*`, ...) are answered alongside.
- Jobs that change skills (all but `skills.check` and `sources.discover`) hold the mutation lock that the command line
  uses (`docs/formats.md`, section 8). When another process holds it, the request gets error `1002` at once. Methods that
  are not jobs but change what jobs read (`providers.addCustom`, `providers.removeCustom`, `automation.set`) follow the
  same two rules.
- `event.progress` counts skills: `done` of `total` are through, `current` is the repository that just finished.
- `event.jobDone` carries `error` when the job as a whole failed (for example "no SKILL.md found in owner/repo").

## Methods

Parameters marked `?` are optional. `names` is a list of skill names; an empty or missing list means all.

| Method | Parameters | Result |
|---|---|---|
| `skills.check` | `names?` | job; the results carry the new statuses |
| `skills.update` | `names?`, `stage?` (a reviewed stage from `skills.diff`), `auto?` (honour holds and `autoSkipScripts`) | job |
| `skills.install` | `source` (any input `sources.discover` accepts), `skills` (names), `mode?` (`copy` or `link` for local folders) | job |
| `skills.remove` | `name` | job |
| `skills.track` | `name`, `source`, `path?` | job |
| `skills.read` | `name` | `{ "name", "text", "truncated" }`: the first 80 KB of its SKILL.md, for the preview |
| `skills.diff` | `name` | `{ "stage", "treeSha", "localEdits", "files":[{"path","status","add","del","binary","exec","risk"}], "patch", "truncated" }` |
| `skills.history` | `name` | `{ "versions":[{"stamp","reason","createdAt","treeSha","hasLockEntry"}], "hold": {...} }` |
| `skills.rollback` | `name`, `stamp?` (newest when missing) | job |
| `skills.restore` | `name` (from `_removed/`) | job |
| `skills.hold` | `name`, `on` | `{}` |
| `skills.setAgentEnabled` | `name`, `agent` (agent id, or `@shared`), `enabled` | job |
| `sources.discover` | `input` | job; the results carry `candidates:[{"name","description","dir","installed"}]`, `host`, `sourceType` |
| `adopt` | none | job |
| `relink` | none | job |
| `doctor` | `fix?` | `{ "problems":[...], "fixed":N }`; with `fix` it is a job |
| `providers.list` | none | `{ "providers":[ <Provider>, ... ] }` |
| `providers.setManaged` | `id`, `on` | job (switching on links skills into the agent) |
| `providers.addCustom` | `label`, `dir`, `readsShared` | `{ "id" }`, or error `1003` with the reason |
| `providers.removeCustom` | `id` | `{}` |
| `providers.rescan` | none | `{ "managed":N, "known":N }` |
| `transfer.export` | `path`, `includeLocal` | `{ "tracked":N, "local":N, "skipped":[...] }` |
| `transfer.preview` | `path` | `{ "items":[{"name","description","local","source","files","exists","problem","installable"}] }` |
| `transfer.import` | `path`, `names` | job |
| `automation.status` | none | `{ "supported":true, "timer":"on", "watcher":"off", "mechanism":"systemd" }` |
| `automation.set` | `timer?`, `watcher?` (booleans) | `{ ... as automation.status }` |
| `projects.list` | none | `{ "projects":[ <Project>, ... ], "suggested":[...] }` |
| `projects.add` | `path` | `{ <Project> }` |
| `projects.remove` | `path` | `{}` |
| `scope.set` | `scope` | snapshot page 0 |
| `settings.get` | none | the whole `settings.json` object |
| `settings.set` | `patch` (an object merged into the settings; a `null` value removes that key) | the whole object; the backend also sends `settings.changed`. Settings are not skills, so this needs no mutation lock and works while a job runs |
| `system.info` | none | `{ "user","host","os","kernel","shell","terminal","uptime","paths":{...} }` |
| `paths.sizes` | none | job; results `{ "skills":bytes, "backups":bytes, "cache":bytes, "disk":{"total":bytes,"free":bytes} }` |
| `fetch.render` | `cols`, `colors` (`truecolor` or `256`), `icons?` | `{ "ansi": "<the banner as terminal text>" }` |
| `palette.derive` | `seed` (`#RRGGBB`), `mode` (`dark` or `light`) | `{ "roles":{"action","actionInk","mark","markText","markLow","focus","selected","inkOk"}, "clash":"" }` |
| `jobs.cancel` | `job` | `{}` |

`<Project>`: `{ "path", "name", "skills":N, "problems":N, "autoUpdate":false }`.

What 2.0's backend answers today (milestone M1c): `initialize`, `shutdown`, `state.snapshot`, `skills.read`,
`skills.check`, `skills.update`, `skills.install`, `skills.remove`, `skills.track`, `sources.discover`, `adopt`, `relink`,
`providers.list`, `providers.setManaged`, `providers.addCustom`, `providers.removeCustom`, `providers.rescan`,
`transfer.export`, `transfer.preview`, `transfer.import`, `automation.status`, `automation.set`, `settings.get`,
`settings.set` and `jobs.cancel`. The others arrive with the features they belong to (diff, history and rollback;
per-agent switches; projects; the terminal and `fetch`; the accent engine) and until then get `-32601`.

### Notifications from the backend

| Method | When |
|---|---|
| `state.delta` | state changed (a job, or a change made outside, seen by the file watchers) |
| `state.resync` | deltas were dropped; take a snapshot |
| `event.progress`, `event.log`, `event.jobDone` | job reporting |
| `settings.changed` | `settings.json` changed (by this or another process) |
| `lock.split` | right after `initialize`, when both user lock files exist (see `docs/formats.md` section 2): `{"primary","other","onlyInOther"}`; the window shows the "Merge" banner |

## File watching

The backend watches the shared folder, every managed agent folder and the lock file. Events only mark state as dirty.
Changes the backend made itself are recognized by a mutation counter and ignored. After each commit, and 300 ms after the
last outside change, it rescans what is dirty and sends one delta. If the watcher reports an overflow, everything is
rescanned.

## Errors

JSON-RPC codes `-32700` (parse error), `-32600` (invalid request), `-32601` (unknown method), `-32602` (invalid
parameters), plus:

| Code | Meaning |
|---|---|
| `-32002` | protocol versions differ |
| `1001` | busy: a job that changes files is running |
| `1002` | locked: another process (a command line run, a timer) holds the mutation lock |
| `1003` | refused, with a reason in `message` (invalid name or path, folder too broad, ...) |
| `1004` | not found |
| `1005` | conflict: the file changed since it was read (for example the lock file) |
| `1006` | cancelled |
| `1007` | a file could not be parsed, so it is not written (`docs/formats.md` section 2) |

Every error has a human-readable `message`, written as the window would show it.
