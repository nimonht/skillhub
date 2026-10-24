#!/bin/sh
# Offline smoke test: adopt / relink / doctor / list / remove against a throw-away $HOME.
# usage: tests/smoke.sh [path/to/skillhub]
set -eu
BIN=$(cd "$(dirname "${1:-build/skillhub}")" && pwd)/$(basename "${1:-build/skillhub}")
HERE=$(cd "$(dirname "$0")" && pwd)
python3 -I "$HERE/catalog-check.py" || exit 1
H=$(mktemp -d)
trap 'rm -rf "$H"' EXIT
export HOME="$H" XDG_CACHE_HOME="$H/.cache" XDG_CONFIG_HOME="$H/.config"
# stub systemctl: the test must never talk to the real user manager
mkdir -p "$H/stubbin"
printf '#!/bin/sh\necho "$*" >> "%s/systemctl.log"\nexit 0\n' "$H" > "$H/stubbin/systemctl"
chmod +x "$H/stubbin/systemctl"
export PATH="$H/stubbin:$PATH"
# agent detection looks for commands only here, never in the real PATH: a fake `codex` and `agy` are "installed"
mkdir -p "$H/provbin"
for c in codex agy; do printf '#!/bin/sh\nexit 0\n' > "$H/provbin/$c"; chmod +x "$H/provbin/$c"; done
export SKILLHUB_BIN_PATH="$H/provbin"
fail() { echo "FAIL: $*" >&2; exit 1; }
skill() { mkdir -p "$1"; printf -- '---\nname: %s\ndescription: %s\n---\n\nbody %s\n' "$(basename "$1")" "${2:-test skill}" "${3:-}" > "$1/SKILL.md"; }

# agents "installed" through their own folders/files (Claude Code, Gemini CLI); Codex and Antigravity through commands
mkdir -p "$H/.claude" "$H/.codex"
mkdir -p "$H/.gemini"; printf '{}' > "$H/.gemini/settings.json"

# one good central skill, a few strays, and entries that must be left alone
skill "$H/.agents/skills/alpha"
skill "$H/.claude/skills/beta"                       # stray in Claude's dir
skill "$H/.codex/skills/gamma"                       # stray in Codex's dir
skill "$H/.codex/skills/alpha"                       # identical duplicate of a central skill
skill "$H/.gemini/skills/alpha" "different text" x   # conflicting copy
skill "$H/.codex/skills/.system/builtin"             # tool-owned, must be ignored
skill "$H/.claude/skills/synced/cloud"               # tool-owned, must be ignored

"$BIN" doctor >/dev/null && fail "doctor should report problems before adopt"
"$BIN" adopt >/dev/null

for n in alpha beta gamma; do
  [ -f "$H/.agents/skills/$n/SKILL.md" ] || fail "$n missing from central dir"
  [ -L "$H/.claude/skills/$n" ] || fail "$n not linked into Claude's dir"
  [ "$(readlink "$H/.claude/skills/$n")" = "../../.agents/skills/$n" ] || fail "$n link is not relative"
done
[ ! -e "$H/.codex/skills/gamma" ] || fail "stray left in Codex dir"
[ ! -e "$H/.codex/skills/alpha" ] || fail "duplicate left in Codex dir"
[ ! -e "$H/.gemini/skills/alpha" ] || fail "conflicting copy left in Gemini dir"
[ -f "$H/.codex/skills/.system/builtin/SKILL.md" ] || fail ".system was touched"
[ -f "$H/.claude/skills/synced/cloud/SKILL.md" ] || fail "synced/ was touched"
grep -q "body" "$H/.agents/skills/alpha/SKILL.md" && grep -q "test skill" "$H/.agents/skills/alpha/SKILL.md" || fail "central alpha was overwritten"
ls "$H/.agents/.backup/_conflicts" >/dev/null 2>&1 || fail "conflicting copy was not backed up"

"$BIN" doctor >/dev/null || fail "doctor should be healthy after adopt"
[ "$("$BIN" adopt | tail -1)" = "0 change(s)" ] || fail "adopt is not idempotent"
"$BIN" list --json | python3 -c 'import json,sys; d=json.load(sys.stdin); assert sorted(x["name"] for x in d)==["alpha","beta","gamma"], d' || fail "list --json"

"$BIN" remove beta >/dev/null
[ ! -e "$H/.agents/skills/beta" ] && [ ! -L "$H/.claude/skills/beta" ] || fail "remove left files behind"
ls "$H/.agents/.backup/_removed" | grep -q '^beta-' || fail "remove did not keep a backup"

# a dead link is cleaned up
ln -s ../../.agents/skills/nope "$H/.claude/skills/nope"
"$BIN" adopt >/dev/null
[ ! -L "$H/.claude/skills/nope" ] || fail "dead link survived"
# ---- providers: detection, overrides, custom agents --------------------------------------------------------------------
"$BIN" providers --json | python3 -c '
import json, sys
d = {p["id"]: p for p in json.load(sys.stdin)}
assert d["claude-code"]["managed"] and d["claude-code"]["mode"] == "link", d
assert d["codex"]["managed"] and d["codex"]["mode"] == "native", d
assert d["gemini-cli"]["managed"] and d["gemini-cli"]["mode"] == "native", d
assert d["antigravity"]["managed"] and d["antigravity"]["mode"] == "config", d
assert "cursor" not in d and "windsurf" not in d, "agents that are not installed must not be managed"
' || fail "providers --json"
[ -f "$H/.gemini/config/skills.json" ] && grep -q "$H/.agents/skills" "$H/.gemini/config/skills.json" || fail "Antigravity was not pointed at the shared folder"
[ ! -e "$H/.codex/skills/alpha" ] && [ ! -e "$H/.codeium" ] && [ ! -e "$H/.cursor" ] || fail "something was written for an agent that reads the shared folder or is not installed"

# an agent that is not installed is left alone ...
mkdir -p "$H/.codeium/windsurf/skills/ghost"; printf -- '---\nname: ghost\ndescription: x\n---\n' > "$H/.codeium/windsurf/skills/ghost/SKILL.md"
rm -rf "$H/.codeium"
"$BIN" adopt >/dev/null
[ ! -e "$H/.codeium" ] || fail "an agent that is not installed got files"
# ... until the user switches it on
"$BIN" providers enable windsurf >/dev/null
"$BIN" adopt >/dev/null
[ -L "$H/.codeium/windsurf/skills/alpha" ] || fail "a provider switched on by the user was not linked"
"$BIN" providers disable windsurf >/dev/null
"$BIN" providers --json | python3 -c 'import json,sys; assert "windsurf" not in {p["id"] for p in json.load(sys.stdin)}' || fail "disabled provider still listed as managed"

# a custom agent: managed, its folder adopted, then removed again
skill "$H/.myagent/skills/mine"
"$BIN" providers add "My Agent" "$H/.myagent/skills" >/dev/null
"$BIN" adopt >/dev/null
[ -f "$H/.agents/skills/mine/SKILL.md" ] && [ -L "$H/.myagent/skills/mine" ] && [ -L "$H/.myagent/skills/alpha" ] || fail "custom provider was not adopted/linked"
"$BIN" providers add "Bad" "$H" >/dev/null 2>&1 && fail "a provider folder equal to \$HOME must be refused"
"$BIN" providers remove custom-my-agent >/dev/null
"$BIN" providers --json | grep -q custom-my-agent && fail "custom provider not removed"

# dead links that do not point into the shared folder are not ours to delete
ln -s /nonexistent/elsewhere "$H/.claude/skills/foreign-dead"
"$BIN" adopt >/dev/null
[ -L "$H/.claude/skills/foreign-dead" ] || fail "a foreign dead link was deleted"
rm "$H/.claude/skills/foreign-dead"

# ---- transfer: export here, import on a fresh "machine", and refuse hostile files ---------------------------------------
mkdir -p "$H/.agents/skills/alpha/scripts"; printf '#!/bin/sh\necho hi\n' > "$H/.agents/skills/alpha/scripts/run.sh"; chmod +x "$H/.agents/skills/alpha/scripts/run.sh"
EXPORT="$H/export.json"
"$BIN" export "$EXPORT" >/dev/null
python3 - "$EXPORT" <<'PY' || fail "export file structure"
import json, sys
d = json.load(open(sys.argv[1]))
assert d["format"] == "skillhub-export" and d["version"] == 1
by = {s["name"]: s for s in d["skills"]}
assert {"alpha", "gamma"} <= set(by), set(by)
assert all(s.get("local") for s in d["skills"])          # nothing in this fake home has an upstream
assert any(f["path"] == "scripts/run.sh" and f["exec"] for f in by["alpha"]["files"])
PY
"$BIN" export "$H/nolocal.json" --no-local >/dev/null
python3 -c "import json,sys; assert json.load(open('$H/nolocal.json'))['skills'] == []" || fail "--no-local still exported local skills"

H2=$(mktemp -d); mkdir -p "$H2/.claude"
second() { HOME="$H2" XDG_CACHE_HOME="$H2/.cache" XDG_CONFIG_HOME="$H2/.config" "$@"; }
second "$BIN" import "$EXPORT" >/dev/null 2>&1 && fail "import without --yes and without a terminal must refuse"
second "$BIN" import "$EXPORT" --yes >/dev/null
cmp "$H/.agents/skills/alpha/SKILL.md" "$H2/.agents/skills/alpha/SKILL.md" || fail "imported skill differs"
[ -x "$H2/.agents/skills/alpha/scripts/run.sh" ] || fail "executable bit was lost"
[ -L "$H2/.claude/skills/alpha" ] || fail "imported skill was not linked into the installed agent"
second "$BIN" import "$EXPORT" --yes | grep -q 'nothing to install' || fail "importing twice must skip what is installed"

# a hostile file: unsafe names, paths that climb out, non-GitHub sources, an absolute path, a symlink-looking name
python3 - "$H/hostile.json" <<'PY'
import base64, json, sys
b = lambda t: base64.b64encode(t.encode()).decode()
skills = [
  {"name": "../evil", "local": True, "files": [{"path": "SKILL.md", "data": b("x")}]},
  {"name": "climb", "local": True, "files": [{"path": "SKILL.md", "data": b("x")}, {"path": "../../escaped.txt", "data": b("pwned")}]},
  {"name": "absolute", "local": True, "files": [{"path": "SKILL.md", "data": b("x")}, {"path": "/tmp/skillhub-escaped.txt", "data": b("pwned")}]},
  {"name": "gitdir", "local": True, "files": [{"path": "SKILL.md", "data": b("x")}, {"path": ".git/hooks/pre-commit", "data": b("pwned")}]},
  {"name": "nomd", "local": True, "files": [{"path": "readme.txt", "data": b("x")}]},
  {"name": "badsource", "source": "evil.example.com/x/y", "url": "https://evil.example.com/x/y.git", "path": "skills/badsource/SKILL.md"},
  {"name": "badpath", "source": "owner/repo", "path": "../../etc/passwd"},
]
json.dump({"format": "skillhub-export", "version": 1, "skills": skills}, open(sys.argv[1], "w"))
PY
second "$BIN" import "$H/hostile.json" --yes | grep -c '^!' | grep -q '^7$' || fail "hostile entries were not all rejected"
[ ! -e "$H2/escaped.txt" ] && [ ! -e /tmp/skillhub-escaped.txt ] && [ ! -e "$H2/.agents/skills/climb" ] && [ ! -e "$H2/.agents/skills/gitdir" ] || fail "a hostile import wrote something"
echo '{"format":"other"}' > "$H/other.json"; second "$BIN" import "$H/other.json" --yes >/dev/null 2>&1 && fail "a foreign file was accepted"
rm -rf "$H2"

# uninstall: units, installed files below $HOME and the cache go; skills, lock, links and foreign files stay
IB="$H/.local/bin/skillhub"; HC="$H/.local/share/icons/hicolor"
mkdir -p "$H/.local/bin" "$H/.local/share/applications" "$HC/48x48/apps" "$HC/scalable/apps" "$H/.config/systemd/user" "$XDG_CACHE_HOME/skillhub/repos/x"
cp "$BIN" "$IB"
touch "$H/.local/share/applications/skillhub.desktop" "$H/.local/share/applications/other.desktop" \
      "$HC/48x48/apps/skillhub.png" "$HC/scalable/apps/skillhub.svg" "$HC/48x48/apps/other-app.png" \
      "$H/.config/systemd/user/skillhub-update.timer" "$H/.config/systemd/user/skillhub-update.service" \
      "$H/.config/systemd/user/skillhub-adopt.path" "$H/.config/systemd/user/skillhub-adopt.service" "$H/.config/systemd/user/other.service"
"$IB" uninstall --dry-run >/dev/null
[ -x "$IB" ] && [ -d "$XDG_CACHE_HOME/skillhub" ] && [ -f "$H/.config/systemd/user/skillhub-update.timer" ] || fail "uninstall --dry-run removed something"
"$IB" uninstall --yes >/dev/null
for f in "$IB" "$H/.local/share/applications/skillhub.desktop" "$HC/48x48/apps/skillhub.png" "$HC/scalable/apps/skillhub.svg" \
         "$H/.config/systemd/user/skillhub-update.timer" "$H/.config/systemd/user/skillhub-update.service" \
         "$H/.config/systemd/user/skillhub-adopt.path" "$H/.config/systemd/user/skillhub-adopt.service" "$XDG_CACHE_HOME/skillhub"; do
  [ ! -e "$f" ] || fail "uninstall left $f"
done
for f in "$HC/48x48/apps/other-app.png" "$H/.local/share/applications/other.desktop" "$H/.config/systemd/user/other.service"; do
  [ -e "$f" ] || fail "uninstall removed a file that is not ours: $f"
done
[ -d "$HC/48x48/apps" ] || fail "uninstall removed a shared icon directory"
grep -q 'disable' "$H/systemctl.log" || fail "uninstall did not disable the units"
[ -f "$H/.agents/skills/alpha/SKILL.md" ] && [ -L "$H/.claude/skills/alpha" ] || fail "uninstall touched the skills"
echo "smoke test passed"
