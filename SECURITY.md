# Security

## What Skill Hub does with the network

- `git clone` / `git fetch` over HTTPS for repositories you add or track, into `~/.cache/skillhub/repos`.
- Looking for a new version of Skill Hub: `git ls-remote` on `github.com/nimonht/skillhub` at launch, once a day while the window is open,
  and when you run `skillhub self-update`. GitHub sees your address, as with any git fetch, and nothing else is sent. *Look for new versions
  of Skill Hub* in Settings turns it off for the window.
- Where the program can replace itself: the release's `SHA256SUMS` and its signature while looking, and the new version once you ask for it.
  These come from GitHub's release files, through the system's `curl` or `wget`.

No telemetry, no accounts, no other endpoints.

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
