# Security

## What Skill Hub does with the network

Only `git clone` / `git fetch` over HTTPS for repositories you add or track, into `~/.cache/skillhub/repos`.
No telemetry, no accounts, no other endpoints.

## Import files are untrusted

An export file can come from anywhere, so `skillhub import` validates it before anything is written: skill names and file paths must be plain relative names (no `..`, no absolute paths, no `.git`), file and skill sizes are capped, remote sources must be GitHub `owner/repo` names or plain https URLs with no user name or token (local folders, `file:`, `ssh:` and `git@` sources are refused, and the file's own URLs are ignored), nothing in it is executed, symlinks are never created and an installed skill is never overwritten. It still installs *instructions that your agents follow*: read the preview and import only files you trust.

## Reviewing updates

Comparing a skill with upstream runs `git diff --no-index` with external diff drivers, text conversion and colour off, so your git configuration runs nothing on untrusted files. A reviewed update is applied exactly as shown: the staged folder's hash is checked again first, and upstream moving on in the meantime changes nothing. Automatic runs leave out updates that add or change scripts or executables until you review them, unless you set `updates.autoSkipScripts` to false. The check looks at what is different on disk, so it does not depend on what upstream claims changed. It is a brake, not a guarantee: a skill's `SKILL.md` alone can already tell an agent to do things.

## Trust model

Agent skills are text (and sometimes scripts) that your coding agents follow with **your** permissions. Updating a
skill means accepting whatever the upstream repository's default branch contains at that moment.

- Track only repositories you trust. Review a skill (the GUI shows `SKILL.md`) before first use.
- Prefer `skillhub check` + manual `skillhub update NAME` over the daily timer if you want a review step.
- Previous versions are kept in `~/.agents/.backup/<skill>/` so a bad update can be rolled back by hand.
- Skill names are validated so they cannot escape the skills directory.

## Reporting a vulnerability

Please open a private security advisory on GitHub (Security tab -> "Report a vulnerability") rather than a public issue.
