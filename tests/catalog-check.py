#!/usr/bin/env python3
"""The shipped agent catalog and brand marks are consistent (run by tests/smoke.sh)."""
import json
import os
import sys

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")
cat = json.load(open(os.path.join(root, "providers.json")))["providers"]
icons = json.load(open(os.path.join(root, "provider-icons.json")))["icons"]
ids = [p["id"] for p in cat]
errors = []
if len(ids) != len(set(ids)):
    errors.append("duplicate provider ids")
for p in cat:
    if p.get("mode") not in ("link", "native", "config"):
        errors.append("%s: bad mode" % p["id"])
for pid, ic in icons.items():
    if pid not in ids:
        errors.append("icon for unknown provider " + pid)
    if len(ic.get("vb", [])) != 4 or ic["vb"][0] != 0 or ic["vb"][1] != 0:
        errors.append("%s: viewBox must be 0 0 w h" % pid)
    if not ic.get("paths") or any(not p.get("d") for p in ic["paths"]):
        errors.append("%s: no path data" % pid)
    if len({p["eo"] for p in ic["paths"]}) > 1:
        errors.append("%s: mixed fill rules" % pid)
for must in ("claude-code", "codex", "antigravity", "zed", "cursor", "github-copilot"):
    if must not in icons:
        errors.append("missing mark for " + must)
if errors:
    sys.exit("catalog check failed:\n  " + "\n  ".join(errors))
print("catalog ok: %d agents, %d with a mark" % (len(ids), len(icons)))
