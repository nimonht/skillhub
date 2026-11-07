# Changelog

## 2.0.0-beta6 - 2026-11-07

The last beta before 2.0.0: tools for finding out what went wrong, a terminal that keeps its keys, and the glass of macOS 26. From
beta3 and later this version arrives through the Update button.

- **A log viewer.** The dock has a *Log* tab (`Ctrl+Shift+L`) with every line of the session and the one before: what the window, Qt and the
  background service said. Filter by level (all, warnings, errors) and by side (window or service), search, follow the newest line, wrap long
  lines, select lines and copy them, or save what is shown. The lines are also kept in `logs/skillhub.log` in the cache folder, five files
  of 1 MiB, so they survive a crash; before this, Qt's warnings and the background service's standard error were lost or sat in a file
  nothing showed. The Windows window, which has no console, shows its warnings here for the first time.
- **A diagnostic report.** *Settings, Advanced, Diagnostics* saves or copies a text file with versions, system, graphics, folders,
  settings, memory use of the window and of the background service, and the end of the log; `skillhub diagnose [FILE]` makes the part the
  command line knows. The home folder is written as `~`, user and host names are left out, and nothing is sent anywhere.
- **Memory dumps, by the system's tool.** The same page shows the two process numbers and the steps for Task Manager's *Create memory
  dump file* (and `gcore` or `lldb` elsewhere). Skill Hub does not write dumps itself: an unsigned program that can read memory looks like a
  credential stealer to an antivirus. CI now fails a Windows build whose programs import `MiniDumpWriteDump`, `dbghelp` or the functions
  that read another program's memory.
- **The terminal keeps the keyboard.** A focused terminal owns every key except a few chords of the window: `/` no longer opens the
  search, `Esc` no longer dismisses a panel, and `F5`, `Ctrl+R`, `Ctrl+F`, `Ctrl+N`, `Ctrl+U` and `Ctrl+J` reach the shell. Switching tabs, opening
  the dock or starting a terminal gives it the keyboard; clicking a tab no longer takes it away. On macOS the Control key is a Control key
  again (it was sent as Command), and `Cmd+C` and `Cmd+V` copy and paste.
- **The terminal scrolls.** A touchpad or a finely stepped wheel moved the history by nothing at all (steps under a notch were rounded
  down); they add up now. A shell or a spinner that wrote while you were reading used to pull the view back to the bottom; the view stays
  on what you are reading until you type. In `less`, `vim`, `tmux` and full-screen agents the wheel goes to the program (mouse reports,
  or arrow keys), and `Shift` takes it back. `Shift+PageUp`, `PageDown`, `Home` and `End` move through the history, and a thin bar shows where
  you are.
- **Liquid Glass.** On macOS 26 the window's glass takes the theme's appearance instead of the system's, the effect view is found
  again when the material changes (it was most likely added again each time), and the panels are far more see-through (their floor is 0.74 and 0.62
  instead of 0.89 and 0.83; `tests/contrast.py` still holds every text to 4.5:1). Toasts, the project menu and tooltips are redrawn as glass:
  a lens that bends what is behind at the rim, bright edges lit from the top left, a faint colour fringe, frosting, and a soft shadow. The side
  panels get a lit rim. This was written without a Mac: please report what it looks like on macOS 26.
- **No flicker on hover in the light theme.** Hovering a row, a rail entry, a switch or a button faded its fill from `transparent`, which
  is black without opacity, so every hover flashed dark and moving over a list flickered and felt slow. The fills now fade from their own colour.
- **Windows.** The shell of a terminal starts inside its job (the job-list attribute) instead of suspended and resumed, which is one less
  call of the kind an antivirus is suspicious of. If Microsoft Defender blocks the unsigned programs anyway, the README says what to do;
  signing them is the lasting answer and is not done yet.

## 2.0.0-beta5 - 2026-11-03

The fixes planned as "beta 4.1", and the window in Vietnamese. It is called beta5 because the updater in beta3 and beta4 reads
versions as `2.0.0-betaN`, and would never offer a `beta4.1`; from either of them this version arrives through the Update button.

- **Closing a terminal ends what runs in it.** A program that catches the hangup, such as Codex, kept running after its tab or the
  whole window was closed, out of sight, until the computer was shut down. The shell gives every command a process group of its own,
  and the hangup and the kill reached only the shell's. Now they reach every process in the terminal's session. A program that
  detached on purpose (`setsid`, a daemon, an app with a window of its own such as VS Code from `code .`) keeps running, as in other
  terminals. On Windows the shell runs in a job: console programs it started end with the tab, also helpers on hidden consoles.
- **The accent can be changed again.** In beta4, every colour picked in Settings or in the first-run tour was saved as nothing, and
  from then on the accent buttons did nothing at all. The Qt that the releases ship hands such a choice over in a form that was
  written as `null`. The project folders under Settings → Projects were lost the same way. A `settings.json` written by beta4 works
  again without editing it.
- **Sliders keep what you set.** Every slider in Settings (intensity, hue, font size, downloads at once, the cache limit and the rest)
  jumped back to its old value as soon as anything else changed: the value it saved was the one from before the drag. Now what the
  knob shows when you let go is saved, once.
- **Tiếng Việt.** The window speaks Vietnamese: Settings → Appearance → Language, or the first page of the tour. It follows the
  system's language until you choose, and switches at once. What the background service and the command line say (the activity
  log, error details) stays in English for now. `i18n/vi.json` holds the translation, and CONTRIBUTING.md explains how to add a
  language.

## 2.0.0-beta4 - 2026-11-03

Know what a skill does before an agent follows it. From beta3 this version arrives through the update button.

- **An offline check of every skill.** `skillhub scan`, a skill's new Checks section and Review read every text file with fixed rules:
  downloads piped into a shell, commands that wipe files or disks, invisible or reordered characters, drop services for data,
  instructions to override the agent's rules or hide things from you, reads of credential files, long encoded blobs. Each finding has
  a level, a file and a line. The rules were tuned until Anthropic's and Vercel's official skills show nothing high. A finding is
  something to read; no finding is no proof of safety.
- **Security reports from skills.sh, if you want them.** Settings → Safety turns them on (off by default), and `skillhub scan
  --reports` asks once. Each of the three scanners is shown with its result and date, never added up: they disagree, Socket for
  example calls some of Anthropic's own skills critical. Only public GitHub repositories are looked up, checked without your
  credentials, so a private one is never sent. `DO_NOT_TRACK` switches reports off.
- **Automatic runs wait for you** when an update brings a new high finding, or when a scanner's report got worse since the last
  update. Review shows why; a manual update still goes through. Settings → Safety turns this off.
- **A second opinion from your own agent.** Claude Code, Codex or Gemini CLI opens in the built-in terminal on a copy of the skill,
  never in its folder, with the command typed for you to start. Nothing it says is stored.

## 2.0.0-beta3 - 2026-11-02

Install this version by hand once: earlier versions have no updater. From here on it updates itself.

- **Skill Hub updates itself.** The Windows installer, the macOS app and the Linux tarball show *Update to X* in the status bar when a new
  version is out, and one click downloads it, checks it and restarts. `skillhub self-update` does the same in a terminal, and
  `skillhub self-update --check` only looks. Betas follow betas, final versions follow final versions, and it never moves backwards.
- **A tarball for Linux, without root.** `skillhub-<version>-linux-<arch>.tar.gz` brings its own Qt, installs below `~/.local` with
  `install.sh`, and is the Linux install that updates itself.
- **A notice everywhere else.** The `.deb`, `.rpm` and PKGBUILD are not in a repository, so `apt` and `dnf` do not find a new version by
  themselves. Those installs, source builds and the Windows zip now say when one is out and what to do.
- **Releases are signed.** The programs are not code-signed, so the updater installs only what carries a signature by a key that stays
  offline, and a release without one does not exist for it. [SECURITY.md](SECURITY.md) has the details.
- **One more thing on the network.** The window now reads the list of Skill Hub's versions from GitHub at launch and once a day. Nothing
  else is sent, nothing is installed until you ask, and Settings turns it off.

## 2.0.0-beta2 - 2026-11-02

- **The Windows build starts.** The Windows zip and installer of 2.0.0-beta1 held Debug builds. `skillhub.exe` stopped at once with
  "Qt6Guid.dll was not found", and `skillhub-cli.exe` needed a debug runtime that only Visual Studio installs. Those two files are gone from
  the beta1 release. Windows builds from source had the same fault and are now Release builds too, unless you ask for another type.
- **Terminal numbers.** A new terminal takes the lowest number no open terminal shows, so closing Terminal 1 and opening another gives
  Terminal 1 again. Open tabs keep their numbers.

## 2.0.0-beta1 - 2026-11-02

A beta: the first public build of 2.0. Please report what breaks. It installs over 1.x, and the final 2.0.0 installs over it.

The core is now C, and the window talks to it over a pipe. `skillhub list` peaks under 4 MB instead of 35, and 10,000 skills install in
5 s, check in under 2 s and update in under 3 s, with the window staying responsive throughout.

Going back to 1.x: run `skillhub enable --all-parked` first, because 1.x knows nothing of parked skills. Everything else 2.0 adds
(sidecars, toggles.json, state.json) is ignored by 1.x and does no harm. The 1.x code stays on the `archive/v1` branch.

- **Space stays under control.** Cached clones no longer grow with every update, and the cache has a limit (1 GB by default); the clones used
  longest ago go first and come back when needed. Removed skills age out after 90 days. Settings → Storage and `skillhub clean` show what uses
  space and free it. Conflicts hold your own edits, so they go only when you ask.

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
- The application menu entry starts the installed program by its full path, so Skill Hub opens from KDE Plasma's launcher even when
  `~/.local/bin` is not on the session's PATH. Shells started from the window find `skillhub` either way.
- New terminals have one greeting. Settings → Terminal chooses Skill Hub's banner, your shell's own, or Automatic, which leaves it to
  the shell when its startup files run fastfetch or the like. With Skill Hub's banner, zsh, bash and fish start with that tool quiet,
  and your startup files stay as they are. Outside terminals follow the same choice.
- Tooltips draw above every panel and stay inside the window. Toasts and tooltips follow the theme instead of an inverted surface.
- Settings, Review and the floating detail panel keep hover, clicks and the wheel from reaching what lies below them.
- A program in the built-in terminal can no longer freeze the window. Writing to a shell that was not reading could stop the whole
  interface for good, for example when printing a file full of terminal queries or pasting a lot of text.
- A password or token inside an `https://` URL is no longer kept: it would have reached the lock file, which projects commit, and the
  logs. Use a git credential helper or an SSH key.
- A clone cut off half way (a timeout, a power loss) is cloned again instead of failing until the cache was cleared by hand.
- A `skillPath` in a project's lock file can no longer lead out of the project for a local source, and a link to a FIFO or a terminal
  no longer blocks a scan.
- `skillhub uninstall` keeps a folder chosen with `SKILLHUB_CACHE`, which may hold more than this program's files. Backups stay in
  order when the clock runs back, and a failed write of the lock file or a hold is now logged instead of passing in silence.

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
