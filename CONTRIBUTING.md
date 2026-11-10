# Contributing

Thanks for helping. The project is small on purpose: a core and command line in C (`skillhub`), and a Qt 6 / C++17 window
(`skillhub-gui`) that talks to it over a pipe through `skillhub serve --stdio` (see `docs/formats.md` and `docs/protocol.md`).

## Build and test

```sh
cmake -B build && cmake --build build -j"$(nproc)"
ctest --test-dir build                        # C unit tests (tests/unit)
sh tests/smoke.sh build/skillhub              # offline: adopt / relink / doctor / remove / uninstall (stubbed systemctl)
python3 tests/portable_smoke.py build/skillhub  # every platform: detection, links, git install / update, export / import
python3 tests/rpc_smoke.py build/skillhub     # the window's backend: skillhub serve --stdio
python3 tests/sources_smoke.py build/skillhub # GitLab, git, branches, local folders, live links, hostile imports
python3 tests/review_smoke.py build/skillhub  # diff, reviewed updates, what automatic runs leave out, rollback, holds, restore
python3 tests/toggles_smoke.py build/skillhub # per-agent switches, parked skills
python3 tests/projects_smoke.py build/skillhub # project skills, hostile lock files, the registry
python3 tests/automation_smoke.py build/skillhub # systemd, launchd and Task Scheduler in a dry run, the resident watcher
python3 tests/parallel_smoke.py build/skillhub # many repositories at once: the parallel engine under load
python3 tests/storage_smoke.py build/skillhub # clones that do not grow, the cache limit, old backups, skillhub clean
sh tests/install-roundtrip.sh                 # cmake --install / uninstall into a fake root; shared dirs must survive
python3 tests/contrast.py build/skillhub      # WCAG gate on qml/Theme.qml, 72 hues of accent, tint floors of the materials
sh tests/gui-smoke.sh build/skillhub out      # headless render of every page, the tour and the terminal; fails on QML errors
python3 tests/greeting_smoke.py build/skillhub # one greeting in a new terminal, on the real zsh, bash and fish
python3 tests/i18n_check.py                   # every text of the window translated, and no English outside qsTr()
qmltestrunner -platform offscreen -input tests/qml  # QML components on their own: the sliders, the hover fades (the Qt 6 one: /usr/lib/qt6/bin on some systems)
python3 tests/shortcut_lint.py                # every shortcut of the window is kept from a focused terminal, or left to it, on purpose
ctest --test-dir build                        # also: the terminal's scrolling and keys with real events (terminal), the view of the history (scroll)
python3 tests/deployed_imports.py --forbidden-only build   # Windows: our programs read no other program's memory and write no dump
```

The C core also builds with sanitizers (`CC=clang cmake -B build-asan -DSKILLHUB_GUI=OFF -DSKILLHUB_SANITIZE=ON`; ThreadSanitizer
through `-DCMAKE_C_FLAGS=-fsanitize=thread -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread`) and with libFuzzer targets for every
parser of untrusted input (`-DSKILLHUB_FUZZ=ON`, see `tests/fuzz`). CI runs the test suites under ASan, UBSan and ThreadSanitizer.
`tools/demo-screenshots.sh` remakes the README pictures from a demo home; it is optional, and only worth running when the window
changes. `tools/gen-unicode.py` and `tools/make-icns.py` regenerate checked-in files. `packaging/check-version.sh` checks that every
file names the same version: change them all together. A beta is `-beta1` in `CMakeLists.txt` and `CHANGELOG.md`, `2.0.0~beta1` in the Debian,
Fedora and AppStream files so that it sorts before 2.0.0, and `2.0.0beta1` in the PKGBUILD. A tag with a hyphen is published as a pre-release.
After a release is published, `sh tools/sign-release.sh vVERSION` signs its `SHA256SUMS` with the offline key (`tools/update-key.sh` made it, once):
until then the updater ignores the release. `tests/selfupdate_smoke.py` runs against `skillhub-update-test`, a build that trusts a test key and must never ship.

### What installed copies depend on

Every copy since 2.0.0-beta3 updates itself with the code it was shipped with, which no later release can change. A release reaches all
of them only while these stay as they are:

- **The tag** is `vX.Y.Z` or `vX.Y.Z-labelN` (lowercase letters, then an optional number): `v2.1.0-beta2`, `v2.1.0-rc1`. Anything else, such as
  `v2.1.0-beta4.1`, is invisible to every installed copy. `packaging/check-version.sh VERSION` refuses it.
- **The files** keep their names: `SkillHub-VERSION-windows-x64-setup.exe`, `SkillHub-VERSION-macos-universal.dmg`,
  `skillhub-VERSION-linux-x86_64.tar.gz` and `skillhub-VERSION-linux-aarch64.tar.gz`, listed in `SHA256SUMS` beside `SHA256SUMS.sig`.
  `packaging/check-release-files.sh` fails the release workflow when one is missing.
- **The key** is the one in `data/update-key.pub`. A release signed with another key does not exist for them.
- **The programs**: `skillhub --version` prints `skillhub VERSION`; the tarball unpacks to one folder with `bin/skillhub` and
  `bin/skillhub-gui`; the disk image holds `Skill Hub.app`; the installer keeps its `AppId` and accepts
  `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /FORCECLOSEAPPLICATIONS /RELAUNCH=1`.
- **Every release installs over any older one**, since a copy moves straight to the newest version without the ones between.
- **A final version reads what a later beta wrote**: from 2.0.0 on, someone on the beta channel can go back to the newest final version
  (never below `SH_UPDATE_FLOOR`, 2.0.0), so a beta that changes `settings.json`, `state.json` or the lock file must leave them readable
  by the final version before it: add keys, do not change the meaning of old ones.

Look at the UI without a display: `QT_QPA_PLATFORM=offscreen build/skillhub gui --shot out.png --theme light --size 1000x640
--select NAME` (also `--agent ID`, `--mode add|review|settings|terminal|onboarding`, `--section NAME`, `--step N`, `--coach N`,
`--scope FOLDER`, `--layout rail=64,detail=460,dock=260`, `--filter problems`, `--search TEXT`, `--discover OWNER/REPO`, `--console`, `--mode log`).
The software renderer used by `offscreen` is a little rougher than the GPU one; check final looks on a real session. It has no shader, so Liquid
Glass shows as a plain surface there: on a machine with a GPU, `SKILLHUB_GLASS_SHOT=1 QT_QPA_PLATFORM=xcb build/skillhub gui --shot out.png
--action adopt` renders the real glass in a toast (`SKILLHUB_FORCE_TRANSLUCENT=1` and a settings.json with `"material":"glass"` choose it).

## Layout

| Path | What |
|---|---|
| `src/cli/`, `src/core/` | the C command line and the C core: store, lock file, parallel git engine (`sh_engine`, `sh_git`), JSON-RPC framing |
| `src/serve/` | `skillhub serve --stdio`, the window's backend: the libuv loop, jobs, state and deltas, file watchers |
| `src/app.*`, `src/gui/` | the window's list models and `AppController`, on top of `CoreClient`, its connection to `skillhub serve` |
| `src/gui/terminal/`, `src/term/` | the built-in terminal: the Qt item over libvterm, and PTY or ConPTY |
| `src/gui/window_effects*` | native blur and Liquid Glass (`NSGlassEffectView` on macOS 26), and `shaders/glass.frag`, the glass of toasts, menus and tooltips |
| `src/gui/app_log.*` | the window's log: a model for the viewer and the rotating file `logs/skillhub.log` |
| `src/core/sh_diag.*` | the diagnostic report (`skillhub diagnose`, `diag.report`) |
| `src/main.cpp` | `skillhub-gui`: start-up and the `--shot` screenshot harness |
| `qml/` | the interface; `Theme.qml` holds every colour, size and duration as roles |
| `i18n/` | the window's languages besides English: `vi.json` maps each English text to Vietnamese (`src/gui/translator.cpp` reads it) |
| `data/providers.json` | the agent catalog, generated by `tools/gen-providers.py` from the skills CLI registry (plus overrides) |
| `data/provider-icons.json` | brand marks of the agents, generated by `tools/gen-provider-icons.py`; check with `tools/icon-sheet.qml` |
| `fonts/` | bundled Barlow (SIL OFL) |
| `DESIGN.md` | the design system: tokens, shapes, rules. Read it before changing the look |
| `tests/` | `smoke.sh` and `install-roundtrip.sh` (Linux), `portable_smoke.py`, `gui_launch.py`, `scan_smoke.py` and `selfupdate_smoke.py` (every platform), `deployed_imports.py` (the Windows package), contrast gate, catalog check |
| `packaging/` | `debian/`, `fedora/skillhub.spec`, `arch/PKGBUILD`, `windows/skillhub.iss`, `macos/make-dmg.sh`, `linux/` (the portable tarball), and the container scripts that build and verify the Linux packages |
| `branding/` | logo SVG masters; `branding/export.sh` rebuilds PNG/ICO/hicolor icons and the banner |

## Adding or fixing an agent

Agents are data, not code. `data/providers.json` is generated: edit the override tables in `tools/gen-providers.py` (commands that
prove an agent is installed, strong evidence for agents that share a config folder, agents that read `~/.agents/skills`
themselves) and regenerate with `python3 tools/gen-providers.py path/to/skills/src/agents.ts <commit> > data/providers.json`.
An agent either reads `~/.agents/skills` natively (`mode: native`), needs a symlink per skill in its own folder (`link`), or is
wired through a config file (`config`, currently Antigravity only). Please link the agent's documentation for where it reads
user-level skills when you open the PR, and keep command-based detection to unambiguous names.

An agent's mark is optional: add it to the table in `tools/gen-provider-icons.py` (marks of the product itself only, never a
vendor logo as a stand-in), regenerate, and look at the result with `tools/icon-sheet.qml` (the command is in its header) before
you commit. Agents without a mark show a two-letter tile.

## Translating the window

Text the window shows is written in English inside `qsTr("...")` in QML and `tr("...")` in C++, with `%1`, `%2` for what is filled
in (`qsTr("%1 skills").arg(n)`), never glued together from pieces, since other languages order words differently. A count gets one
text for one and one for many (`"1 skill"`, `"%1 skills"`). `i18n/<code>.json` maps each English text to its translation; when two
places mean different things by the same English, the second one says so (`qsTr("Clear", "selection")`, `"selection|Clear"` in
the file). `python3 tests/i18n_check.py --missing` prints what a file lacks, and the check fails CI on anything missing, stale, with
other placeholders, or English shown outside `qsTr()`. Names stay as they are (Skill Hub, kitty): end such a line with `// i18n-ok`.
A new language is a new file in `i18n/`, a line in `resources.qrc` and its name in `AppController::languages()`.

## Platforms and Qt versions

The program builds on Linux, macOS and Windows with Qt 6.4 or newer. Code that differs by OS lives in the C core, behind `#ifdef _WIN32` / `__APPLE__` in `src/core` (links in `sh_link.c`, processes in `sh_proc.c`, files in `sh_fs.c`); QML that
needs a newer Qt than 6.4 must have a fallback: `CurveShape.qml` (curve renderer, Qt 6.6) and `NumText.qml` (font features,
Qt 6.7) show the two patterns (a runtime check, and a `qml/+legacy` file chosen by a file selector). Test an old and a new Qt:

```sh
docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/out ubuntu:24.04 sh /src/packaging/container-deb.sh    # Qt 6.4
docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/pkg:ro ubuntu:24.04 sh /src/packaging/container-verify-deb.sh
```

## Style

Match the surrounding code. The C core stays free of Qt. Never delete user data: move it to the backup folder.
In QML, use `Theme` roles instead of literal colours, keep text at 12 px or more, give every state an icon or a word as well
as a colour, and make anything clickable reachable by keyboard.
