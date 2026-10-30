#!/bin/sh
# The README's pictures, from a made-up home folder: every skill, agent and repository in them is demo data.
# usage: tools/demo-screenshots.sh [path/to/skillhub]
set -eu
BIN=$(cd "$(dirname "${1:-build/skillhub}")" && pwd)/$(basename "${1:-build/skillhub}")
ROOT=$(cd "$(dirname "$0")/.." && pwd)
H=$(mktemp -d)
trap 'rm -rf "$H"' EXIT
export HOME="$H" XDG_CACHE_HOME="$H/.cache" XDG_CONFIG_HOME="$H/.config" XDG_STATE_HOME="$H/.local/state" XDG_DATA_HOME="$H/.local/share"
export QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software QT_FORCE_STDERR_LOGGING=1 SHELL=/bin/sh
mkdir -p "$H/provbin" "$H/.claude" "$H/.cursor" "$H/up/acme/field-kit.git" "$H/work/skills"
for c in codex gemini cursor-agent; do printf '#!/bin/sh\nexit 0\n' > "$H/provbin/$c"; chmod +x "$H/provbin/$c"; done
export SKILLHUB_BIN_PATH="$H/provbin"
printf '[url "file://%s/up/"]\n\tinsteadOf = https://github.com/\n[protocol "file"]\n\tallow = always\n[user]\n\tname = demo\n\temail = demo@example.com\n' "$H" > "$H/gitconfig"
export GIT_CONFIG_GLOBAL="$H/gitconfig"
skill() {  # folder name description
  mkdir -p "$1"
  printf -- '---\nname: %s\ndescription: %s\n---\n\n# %s\n\n%s\n\n- Read the task first\n- Keep the change small\n' "$2" "$3" "$2" "$3" > "$1/SKILL.md"
}
git -C "$H/up/acme/field-kit.git" init -q --bare
git -C "$H/work" init -q .
git -C "$H/work" remote add origin "$H/up/acme/field-kit.git"
skill "$H/work/skills/changelog-writer" changelog-writer "Turns merged pull requests into a changelog people want to read"
skill "$H/work/skills/commit-style" commit-style "Writes commit messages in the style of the repository"
skill "$H/work/skills/db-migrations" db-migrations "Plans schema changes that are safe to roll out"
skill "$H/work/skills/pdf-reports" pdf-reports "Fills report templates and exports them as PDF"
skill "$H/work/skills/release-notes" release-notes "Drafts release notes from tags and issues"
skill "$H/work/skills/test-doubles" test-doubles "Chooses between fakes, stubs and mocks"
git -C "$H/work" add -A
git -C "$H/work" commit -q -m one
git -C "$H/work" push -q origin HEAD
"$BIN" add acme/field-kit --all >/dev/null
skill "$H/.agents/skills/team-handbook" team-handbook "What this team agreed on, for every agent to follow"
skill "$H/.agents/skills/ops-runbook" ops-runbook "Steps for the incidents that come back"
"$BIN" adopt >/dev/null
"$BIN" disable team-handbook --agent cursor >/dev/null
rm -f "$H/.claude/skills/ops-runbook"
skill "$H/work/skills/commit-style" commit-style "Writes commit messages in the style of the repository, now with scopes"
printf 'Scopes come from the top folder of the change.\n' >> "$H/work/skills/commit-style/SKILL.md"
git -C "$H/work" add -A
git -C "$H/work" commit -q -m two
git -C "$H/work" push -q origin HEAD
shot() {
  name=$1; shift
  "$BIN" gui --shot "$ROOT/docs/screenshot-$name.png" --size 1440x900 "$@" 2>/dev/null
  echo "ok $name"
}
shot dark --theme dark --select commit-style
shot light --theme light --agent claude-code
shot agents --theme dark --mode providers
shot transfer --theme dark --mode transfer
shot add --theme light --mode add
shot review --theme dark --mode review
shot settings --theme dark --mode settings
shot terminal --theme dark --layout dock=360 --mode terminal --terminal-run 'skillhub --version' --settle 12
