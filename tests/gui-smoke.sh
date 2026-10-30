#!/bin/sh
# Headless GUI check: start the Qt Quick app in a throw-away $HOME, render both themes at three window sizes plus the
# agent and add views, and fail on any QML error or an empty screenshot. Needs no display and no network.
# usage: tests/gui-smoke.sh [path/to/skillhub] [output-dir]
set -eu
BIN=$(cd "$(dirname "${1:-build/skillhub}")" && pwd)/$(basename "${1:-build/skillhub}")
OUT=${2:-$(mktemp -d)}
mkdir -p "$OUT"
H=$(mktemp -d)
trap 'rm -rf "$H"' EXIT
export HOME="$H" XDG_CACHE_HOME="$H/.cache" XDG_CONFIG_HOME="$H/.config" XDG_STATE_HOME="$H/.local/state" XDG_DATA_HOME="$H/.local/share"
# QT_FORCE_STDERR_LOGGING: with stderr not a terminal, Qt may log to journald instead, and QML errors would go unseen
export QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software QT_FORCE_STDERR_LOGGING=1
mkdir -p "$H/provbin"
for c in codex agy; do printf '#!/bin/sh\nexit 0\n' > "$H/provbin/$c"; chmod +x "$H/provbin/$c"; done
export SKILLHUB_BIN_PATH="$H/provbin"   # detect agents from this folder only
mkdir -p "$H/.claude"                  # Claude Code "installed"
fail() { echo "FAIL: $*" >&2; exit 1; }

for n in alpha beta gamma; do
  mkdir -p "$H/.agents/skills/$n"
  printf -- '---\nname: %s\ndescription: demo skill %s for the GUI smoke test\n---\n\n# %s\n\nSome **markdown** with `code` and a [link](https://example.com).\n\n- one\n- two\n' "$n" "$n" "$n" > "$H/.agents/skills/$n/SKILL.md"
done
"$BIN" adopt >/dev/null                 # creates the Claude links
rm "$H/.claude/skills/gamma"            # one visible problem to render

# a repository with an update waiting, a script among the changes, and a version to go back to
mkdir -p "$H/up/acme/demo.git" "$H/work/skills/demo"
git -C "$H/up/acme/demo.git" init -q --bare
printf '[url "file://%s/up/"]\n\tinsteadOf = https://github.com/\n[protocol "file"]\n\tallow = always\n[user]\n\tname = t\n\temail = t@example.com\n' "$H" > "$H/gitconfig"
export GIT_CONFIG_GLOBAL="$H/gitconfig"
git -C "$H/work" init -q .
git -C "$H/work" remote add origin "$H/up/acme/demo.git"
revise() {
  printf -- '---\nname: demo\ndescription: a tracked demo skill\n---\n\n# demo\n\nRevision %s.\n\nThis line stays.\n' "$1" > "$H/work/skills/demo/SKILL.md"
  git -C "$H/work" add -A
  git -C "$H/work" commit -q -m "revision $1"
  git -C "$H/work" push -q origin HEAD
}
revise 1
"$BIN" add acme/demo --all >/dev/null
revise 2
"$BIN" update demo >/dev/null
revise 3
mkdir -p "$H/work/skills/demo/scripts"
printf '#!/bin/sh\necho setup\n' > "$H/work/skills/demo/scripts/setup.sh"
revise 4
"$BIN" disable beta --agent claude-code >/dev/null
"$BIN" disable alpha --shared >/dev/null

shot() {
  name=$1; shift
  "$BIN" gui --shot "$OUT/$name.png" "$@" 2>"$H/err.txt" || { cat "$H/err.txt" >&2; fail "$name: app exited with an error"; }
  if grep -E '\.qml:[0-9]+|TypeError|ReferenceError|Binding loop' "$H/err.txt" >&2; then fail "$name: QML reported errors"; fi
  [ -s "$OUT/$name.png" ] || fail "$name: no screenshot written"
  [ "$(wc -c < "$OUT/$name.png")" -gt 8000 ] || fail "$name: screenshot is nearly empty"
  echo "ok  $name"
}

for theme in dark light; do
  for size in 1280x800 1000x640 820x600; do
    shot "$theme-$size" --theme "$theme" --size "$size" --select alpha
  done
  shot "$theme-agent" --theme "$theme" --size 1280x800 --agent claude-code
  shot "$theme-add" --theme "$theme" --size 1280x800 --mode add --console
  shot "$theme-agents" --theme "$theme" --size 1280x800 --mode providers
  shot "$theme-transfer" --theme "$theme" --size 1280x800 --mode transfer
  shot "$theme-filter" --theme "$theme" --size 1280x800 --filter problems
  shot "$theme-nomatch" --theme "$theme" --size 1280x800 --search zzzz
  shot "$theme-review" --theme "$theme" --size 1280x800 --mode review
  shot "$theme-review-small" --theme "$theme" --size 820x600 --mode review
  shot "$theme-versions" --theme "$theme" --size 1280x800 --select demo
  shot "$theme-switches" --theme "$theme" --size 1280x800 --select beta
  shot "$theme-agent-off" --theme "$theme" --size 1280x800 --agent claude-code
done
echo "gui smoke test passed (screenshots in $OUT)"
