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
export HOME="$H" XDG_CACHE_HOME="$H/.cache" XDG_CONFIG_HOME="$H/.config" QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software
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
done
echo "gui smoke test passed (screenshots in $OUT)"
