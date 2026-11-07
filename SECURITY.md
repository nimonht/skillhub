# Security

## What Skill Hub does with the network

- `git clone` / `git fetch` over HTTPS for repositories you add or track, into `~/.cache/skillhub/repos`.
- Looking for a new version of Skill Hub: `git ls-remote` on `github.com/nimonht/skillhub` at launch, once a day while the window is open,
  and when you run `skillhub self-update`. GitHub sees your address, as with any git fetch, and nothing else is sent. *Look for new versions
  of Skill Hub* in Settings turns it off for the window.
- Where the program can replace itself: the release's `SHA256SUMS` and its signature while looking, and the new version once you ask for it.
  These come from GitHub's release files, through the system's `curl` or `wget`.

- Security reports, **only when you turn them on** (Settings → Safety, or `skillhub scan --reports` for one run): for each skill
  from a **public** GitHub repository, its repository (`owner/repo`) and skill name go to `https://www.skills.sh/tele/audit`,
  the endpoint `npx skills` uses. A repository counts as public only when an anonymous `git ls-remote` of
  `github.com/owner/repo.git` succeeds with your git configuration and credential helpers switched off, so a private
  repository is never sent, even one you can read. `DO_NOT_TRACK` or `DISABLE_TELEMETRY` switches reports off entirely.

No telemetry, no accounts, no other endpoints.

## What Skill Hub keeps on disk for troubleshooting

`logs/skillhub.log` in the cache folder (five files of 1 MiB) holds what the window, Qt and the background service said: the same lines as the
activity feed, Qt's warnings, and what the backend wrote on its standard error. The home folder is written as `~`. It holds no skill text and
no credentials, and it is read by nobody but you: *Settings, Advanced, Diagnostics* and `skillhub diagnose` put the report and the end of the log in
a file or on the clipboard only when you ask, and read no other program. Skill Hub does not write memory dumps and its programs import none of
the functions that would (`tests/deployed_imports.py` checks it for every Windows build); a dump is made with the system's own tool, and can hold
tokens and clipboard text, so share one only with someone you trust.

## Checking skills

`skillhub scan`, the Checks section of a skill and Review read each skill's text files with fixed rules, offline: downloads
piped into a shell, commands that wipe files or disks, invisible or reordered characters, drop services for data, instructions to
override the agent's rules or hide things from you, reads of credential files, long encoded blobs. The rules cannot be talked out
of a result by the skill's text, unlike a model. They find things worth reading; **no finding is no proof of safety**.

Reports from skills.sh are three outside scanners' views. They often disagree (Socket calls some of Anthropic's own skills
critical where the others say safe or low) and describe the skill upstream on their date, not necessarily your version, so
they are shown each on its own and never added up into one verdict.

*Second opinion* opens your own coding agent in the built-in terminal, never in the skill's folder: an agent treats its
working folder as a project and would take the skill's `.claude/`, `.mcp.json` or `AGENTS.md` as its own settings. It works
in a fresh folder that holds a copy of the skill, with the command typed but not run, so the agent asks before it does
anything. What it answers is not stored; a skill's text can steer what an agent concludes.

## Updating Skill Hub itself

The macOS and Windows programs are not code-signed, and a file the program downloads itself carries no quarantine flag, so the system
checks nothing. The updater does it instead:

- Every release's `SHA256SUMS` is signed with an Ed25519 key that stays offline with the maintainer and is **not** on GitHub. The public half is
  built into the program (`data/update-key.pub`). Anyone who takes over the repository or the releases can publish files, but cannot make
  an installed copy accept them.
- A release without a good signature is treated as if it did not exist. The file must match the signed list exactly, and its name carries
  the version, so an old release cannot pass for a new one.
- It never moves to an older version. A final version never moves to a pre-release.
- Before replacing anything, the new program must start and name the version that was signed (Linux and macOS; on Windows the installer does the
  replacing). The old version stays until the next update, or until the next start on macOS.
- A package below `/usr`, a copy built from source and the portable Windows zip are never touched: they get a notice with what to do.

## Import files are untrusted

An export file can come from anywhere, so `skillhub import` validates it before anything is written: skill names and file paths must be plain relative names (no `..`, no absolute paths, no `.git`), file and skill sizes are capped, remote sources must be GitHub `owner/repo` names or plain https URLs with no user name or token (local folders, `file:`, `ssh:` and `git@` sources are refused, and the file's own URLs are ignored), nothing in it is executed, symlinks are never created and an installed skill is never overwritten. It still installs *instructions that your agents follow*: read the preview and import only files you trust.

## Project lock files are untrusted

A `skills-lock.json` comes with a repository you may have just cloned. Skill Hub never runs git inside a project, whose config could name hooks or an fsmonitor. What an entry declares proves nothing, so Skill Hub looks at where it leads once links are followed: a source that resolves to a folder must be inside the project, whatever its `sourceType` says, and a `github` entry must name that repository. A link in the project's shared folder is no skill, and a project whose `.agents/skills` or `skills-lock.json` leads outside it is refused. Remote sources in the file are used when you run `update`, as with `npx skills`, so read a lock file before updating from it; `check` and `diff` change nothing in the project.

## Reviewing updates

Comparing a skill with upstream runs `git diff --no-index` with external diff drivers, text conversion and colour off, so your git configuration runs nothing on untrusted files. A reviewed update is applied exactly as shown: the staged folder's hash is checked again first, and upstream moving on in the meantime changes nothing. Automatic runs leave out updates that add or change scripts or executables until you review them, unless you set `updates.autoSkipScripts` to false. The check looks at what is different on disk, so it does not depend on what upstream claims changed. It is a brake, not a guarantee: a skill's `SKILL.md` alone can already tell an agent to do things.

## Text from repositories

A skill's name, description and files are written by whoever owns the repository, so they are treated as text. The window shows
them as plain text and never fetches anything they name: no images, no style sheets, and links open only for `http`, `https` and
`mailto`, after a click. The command line removes control characters from everything it prints, so a patch cannot hide a line
from the review it appears in. Pasting into the built-in terminal removes escape characters too.

## Trust model

Agent skills are text (and sometimes scripts) that your coding agents follow with **your** permissions. Updating a
skill means accepting whatever the upstream repository's default branch contains at that moment.

- Track only repositories you trust. Review a skill (the GUI shows `SKILL.md`) before first use.
- Prefer `skillhub check` + manual `skillhub update NAME` over the daily timer if you want a review step.
- Previous versions are kept in `~/.agents/.backup/<skill>/` so a bad update can be rolled back by hand.
- Skill names are validated so they cannot escape the skills directory.

## Reporting a vulnerability

Please open a private security advisory on GitHub (Security tab -> "Report a vulnerability") rather than a public issue.
