# Changelog

## 2.0.0 - unreleased

The core is now C, and the window talks to it over a pipe. `skillhub list` peaks under 4 MB instead of 35, and 10,000 skills install in
5 s, check in under 2 s and update in under 3 s, with the window staying responsive throughout.

Going back to 1.x: run `skillhub enable --all-parked` first, because 1.x knows nothing of parked skills. Everything else 2.0 adds
(sidecars, toggles.json, state.json) is ignored by 1.x and does no harm. The 1.x code stays on the `archive/v1` branch.

- **Skills from anywhere.** `skillhub add` and the Add sheet take GitLab projects (nested groups, self-hosted `/-/tree/`
  links), any git URL (https, ssh, `git@host:path`), a branch or tag (`#ref`), a folder inside a repository, and local
  folders, as a copy that updates when you choose or as a live link (`--link`). Input follows `npx skills`, and the lock
  entries are the ones it writes.
- **Review before you update.** `skillhub diff`, `skillhub update --review` and the Review page show every changed file with
  a diff, flag scripts and your own edits, and apply exactly what you read. *Update all* reviews a whole batch, with a tick per
  skill.
- **One-click rollback.** Every backup now records its lock entry, so `skillhub rollback` and *Roll back* bring the source back
  too, and *Undo* follows every update and removal. A rollback sets a hold: automatic updates leave the skill alone until
  upstream moves on (`skillhub hold`, `skillhub release`). `skillhub restore` brings back a removed skill.
- **Per-agent switches.** `skillhub disable` and `enable`, and a switch per agent in the skill's and the agent's detail. Agents
  that read the shared folder switch together: the skill is parked in `~/.agents/.parked`, with its lock entry kept in
  `state.json`, so `npx skills update -g` does not bring it back. Parked skills keep updating. Before going back to 1.x, run
  `skillhub enable --all-parked`.
- **Project skills.** `--project PATH` (or `-C`) on the commands that work on skills, a scope switcher in the window, and
  `skillhub projects`. A project keeps its skills in `.agents/skills`, with relative links for agents that need them and a
  `skills-lock.json` as npx writes it. `skillhub copy` and *Copy to project* bring a skill of yours in, with its source.
- **Daily update and stray watcher on macOS and Windows.** LaunchAgents and scheduled tasks, with `skillhub autoupdate` and
  `skillhub watch` as on Linux. Windows has no file-change trigger, so its watcher is `skillhub watch --run`, started at logon.
  Windows now shows notifications. Scheduled runs use the variables recorded when automation was switched on, so
  `CLAUDE_CONFIG_DIR` and the like resolve as they do in your shell.
- **A terminal in the window.** Any shell, as tabs in the bottom dock, starting in the folder you are working on: libvterm over a
  pty, or ConPTY on Windows. *Open in kitty*, Ghostty, Alacritty, WezTerm and others hands the folder to your own terminal. The look
  can come from your kitty, Ghostty or Alacritty config.
- **`skillhub fetch`**: a banner of the machine and its skills that never touches the network, with `--json`, `--field`, your own
  logo, and a crisp image in kitty, Ghostty and WezTerm.
- **A tour on first start**, on your own agents and skills, with F1 to take it again. A banner offers to merge the two lock files
  when an older Skill Hub and `npx skills` left one each.
- **The window is yours to arrange.** Every panel resizes with the mouse or Ctrl+Alt and the arrows, and keeps its size. The rail
  collapses to icons, the agent band to tiles. A Settings page (Ctrl+,) holds appearance, layout, updates, projects and more.
- **Any accent colour**, your own or your desktop's, with every role derived to keep its contrast. **Blur and Liquid Glass**: native
  on macOS and Windows 11, from your compositor on Linux, down to a tint floor that keeps text readable over anything.
- Automatic updates leave out updates that add or change scripts until you review them (`updates.autoSkipScripts`).
- Export files move to version 2 and carry those sources; version 1 still reads. Import accepts remote sources over https
  only, so a hostile file cannot reach a local folder or use your SSH keys.
- A private repository now says what to set up (a credential helper or an SSH key) instead of only git's message.
- Two removals of the same skill within a second no longer fail.

## 1.0.1 - 2026-10-24

- **Windows: accounts that cannot create symlinks** (the default: Developer Mode off, not an administrator) link agent folders with
  junctions. Those links showed as "OTHER" instead of linked, because Qt cannot resolve junctions; the link target is now read
  from the link itself. CI runs the whole command-line test a second time with symlinks switched off to cover this path.
- macOS 15: the first-start instructions use System Settings, Privacy & Security, Open Anyway.
- `skillhub uninstall` also removes the installed AppStream file; the Arch package checksum is filled in.

## 1.0.0 - 2026-10-24

First stable release.

- **Finds your agents.** A catalog of 75 coding agents that read user-level skills (generated from the skills CLI registry) is
  matched against the machine: an agent's command or its own config folder. Only detected agents are managed; each can be
  switched on or off, and you can add your own. The window, the CLI, the stray watcher and the links all follow the result;
  agents installed later show up when the window regains focus.
- **Brand marks.** Each agent shows its own single-colour mark (Claude Code the Claude spark, Codex, Cursor, Zed, ...) instead of
  initials; 32 of the 75 known agents have one, the rest keep a two-letter tile. Marks come from lobe-icons and simple-icons
  and belong to their owners (see NOTICE.md).
- **Linux, macOS and Windows.** Packages: `.deb` for Debian 12 and 13 and Ubuntu 25.10+, another `.deb` for Ubuntu 24.04 and
  Linux Mint 22, `.rpm` for Fedora 43+ (all amd64 and arm64), a universal macOS disk image, a Windows installer and a portable
  zip, and an Arch PKGBUILD. Qt 6.4 or newer.
  The daily update timer and the stray watcher are systemd units and exist on Linux only; everything else works everywhere.
  macOS and Windows builds are not code-signed.
- **Transfer between machines.** `skillhub export` / Export writes one JSON file (GitHub skills as their source, local skills
  with their files); `skillhub import` / Import shows what is inside and installs everything, never overwriting an installed
  skill. Imports are validated as untrusted input. Drag a file onto the window to start.
- Qt Quick interface: the shared folder next to one column per managed agent above the list, one port per skill in each
  column; side rail with quick filters, tools and automation; detail pane with the rendered `SKILL.md`; agent view with
  one-click repair; agents manager; transfer panel; inline add-from-GitHub sheet; activity log; toasts; empty, loading and
  error states.
- Loaders: one spinner for work of unknown length and a determinate line along the status bar; no sliding bars.
- Dark and light themes that follow the system (or a manual choice), reduced-motion setting, full keyboard operation.
- One canonical skills folder (`~/.agents/skills`) shared by every agent; `adopt` / `doctor` / relink pull strays in, drop
  duplicates, set conflicts aside and clean dead links (only links that point into the shared folder).
- Install from GitHub, per-skill upstream tracking, update check by git tree hash, three backups per skill; compatible with
  the `npx skills` lock file (`~/.agents/.skill-lock.json`, v3).
- systemd user units: daily auto-update timer and a stray-skill path watcher that follows the detected agents.
- `skillhub uninstall` removes units, installed files below `$HOME` and the cache without touching skills; `make uninstall`
  removes only Skill Hub's own files.
- Tests: offline smoke test (adopt, providers, transfer with hostile files), install/uninstall round trip, WCAG contrast gate
  on the design tokens, headless GUI render test of every view in both themes.
