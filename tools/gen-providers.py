#!/usr/bin/env python3
"""Generate data/providers.json (the catalog of coding agents that read "skills") from the agent registry of
https://github.com/vercel-labs/skills (src/agents.ts, MIT licence), plus the small hand-written overrides below.

usage: tools/gen-providers.py /path/to/skills/src/agents.ts [commit] > data/providers.json

Entries without a user-level skills folder, or that can only be detected through the current directory, are skipped.
Path syntax in the output: "~" is the home folder, "${VAR:-default}" is an environment variable with a default.
"""
import json, re, subprocess, sys

src = open(sys.argv[1]).read()
commit = sys.argv[2] if len(sys.argv) > 2 else "unknown"

ENV_HOMES = {  # variable name used in agents.ts -> expression
    "home": "~",
    "configHome": "${XDG_CONFIG_HOME:-~/.config}",
    "codexHome": "${CODEX_HOME:-~/.codex}",
    "claudeHome": "${CLAUDE_CONFIG_DIR:-~/.claude}",
    "vibeHome": "${VIBE_HOME:-~/.vibe}",
    "hermesHome": "${HERMES_HOME:-~/.hermes}",
    "autohandHome": "${AUTOHAND_HOME:-~/.autohand}",
    "grokHome": "${GROK_HOME:-~/.grok}",
    "sarvamHome": "${SARVAM_HOME:-~/.sarvam}",
}

def join_expr(m):
    args = [a.strip() for a in re.split(r",(?![^()]*\))", m)]
    base = ENV_HOMES.get(args[0])
    if base is None:
        return None
    parts = [a.strip("'\"") for a in args[1:]]
    return "/".join([base] + [p for p in parts if p])

def parse_join(text):
    m = re.search(r"join\(([^()]*)\)", text)
    return join_expr(m.group(1)) if m else None

def exists_args(text):
    """Arguments of every existsSync(...) call, with balanced parentheses."""
    out, i = [], 0
    while True:
        i = text.find("existsSync(", i)
        if i < 0:
            return out
        j, depth = i + len("existsSync("), 1
        while j < len(text) and depth:
            depth += {"(": 1, ")": -1}.get(text[j], 0)
            j += 1
        out.append(text[i + len("existsSync("):j - 1].strip())
        i = j

entries = {}
body = src[src.index("export const agents"):]
for m in re.finditer(r"^  ('?)([a-z0-9-]+)\1: \{\n(.*?)^  \},", body, re.S | re.M):
    key, block = m.group(2), m.group(3)
    name = re.search(r"displayName: '([^']+)'", block).group(1)
    g = re.search(r"globalSkillsDir: (.+),\n", block).group(1)
    if g.strip() == "undefined":
        continue
    d = parse_join(g)
    detect = []
    dm = re.search(r"detectInstalled: async \(\) => \{(.*?)\n    \},", block, re.S)
    if dm:
        for e in exists_args(dm.group(1)):
            if "process.cwd" in e or "zedAppDataHome" in e or "zedFlatpak" in e:
                continue
            if e in ENV_HOMES:
                detect.append(ENV_HOMES[e])
            else:
                x = parse_join(e)
                if x:
                    detect.append(x)
    project = re.search(r"skillsDir: '([^']+)'", block)
    entries[key] = {"id": key, "label": name, "dir": d, "projectDir": project.group(1) if project else ".agents/skills", "detect": detect}

# --- hand-written knowledge the registry does not carry ------------------------------------------------------------
def pdir(k): return entries[k]["dir"]
HELPER_DETECT = {"zcode": ["~/.zcode"], "kimchi": ["~/.config/kimchi"], "minimax-code": ["~/.minimax"],
                 "posit-assistant": ["~/.posit/assistant", "~/.positai"], "openclaw": ["~/.openclaw", "~/.clawdbot", "~/.moltbot"]}
for k, v in HELPER_DETECT.items():
    if k in entries:
        entries[k]["detect"] = v
if "openclaw" in entries:
    entries["openclaw"]["dir"] = "~/.openclaw/skills"

# Command-based detection only for unambiguous names (a generic name like `pi`, `cn`, `forge` or `vibe` would be a false positive).
BINS = {"claude-code": ["claude"], "codex": ["codex"], "gemini-cli": ["gemini"], "cursor": ["cursor-agent"],
        "github-copilot": ["copilot"], "opencode": ["opencode"], "windsurf": ["windsurf"], "zed": ["zed", "zeditor"],
        "kiro-cli": ["kiro-cli"], "qwen-code": ["qwen"], "droid": ["droid"], "openhands": ["openhands"], "goose": ["goose"],
        "augment": ["auggie"]}

# Strong evidence for agents whose config folder is shared with another product (here: ~/.gemini is also Antigravity's).
OVERRIDE_DETECT = {"gemini-cli": ["~/.gemini/settings.json", "~/.gemini/oauth_creds.json", "~/.gemini/GEMINI.md"]}
# Agents documented to read ~/.agents/skills themselves, although the registry installs into their own folder.
NATIVE_ALSO = {"gemini-cli": ["~/.gemini/skills"], "droid": ["~/.factory/skills"]}
IGNORE = {"claude-code": ["synced"], "codex": [".system"]}
EXTRA_DIRS = {"codex": ["${CODEX_HOME:-~/.codex}/skills"]}

# One Antigravity (the registry lists the IDE and the CLI separately; both are wired through ~/.gemini/config/skills.json).
for k in ("antigravity", "antigravity-cli"):
    entries.pop(k, None)
entries["antigravity"] = {"id": "antigravity", "label": "Antigravity", "dir": "", "projectDir": ".agents/skills", "mode": "config",
                          "configFile": "~/.gemini/config/skills.json",
                          "detect": ["~/.gemini/antigravity", "~/.gemini/antigravity-cli", "~/.antigravity-ide"],
                          "extraDirs": ["~/.gemini/antigravity/skills", "~/.gemini/antigravity-cli/skills"]}
BINS["antigravity"] = ["agy", "antigravity"]
entries.pop("universal", None)

for k, e in entries.items():
    e.setdefault("mode", "native" if e["dir"] == "~/.agents/skills" else "link")
    if k in OVERRIDE_DETECT: e["detect"] = OVERRIDE_DETECT[k]
    if k in NATIVE_ALSO:
        e["mode"] = "native"
        e["extraDirs"] = NATIVE_ALSO[k]
        e["dir"] = "~/.agents/skills"
    if k in EXTRA_DIRS: e["extraDirs"] = EXTRA_DIRS[k]
    if k in IGNORE: e["ignore"] = IGNORE[k]
    e["bins"] = BINS.get(k, [])
    e.setdefault("extraDirs", [])
    e.setdefault("ignore", [])
    if not e["detect"] and not e["bins"]:
        e["detect"] = [e["dir"].rsplit("/skills", 1)[0]] if e["dir"] else []

# popular agents first, everything else alphabetical
FIRST = ["claude-code", "codex", "gemini-cli", "antigravity", "cursor", "github-copilot", "opencode", "windsurf", "zed", "amp",
         "goose", "kiro-cli", "qwen-code", "cline", "roo", "kilo", "junie", "droid", "openhands", "continue"]
order = [k for k in FIRST if k in entries] + sorted(k for k in entries if k not in FIRST)
out = {"format": "skillhub-providers", "version": 2,
       "source": f"vercel-labs/skills src/agents.ts @ {commit} (MIT) with skillhub overrides",
       "providers": [entries[k] for k in order]}
json.dump(out, sys.stdout, indent=1, ensure_ascii=False)
print()
