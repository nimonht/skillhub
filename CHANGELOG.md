# Changelog

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
