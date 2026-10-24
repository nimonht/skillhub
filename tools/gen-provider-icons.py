#!/usr/bin/env python3
"""Builds data/provider-icons.json: the brand mark of each coding agent, as plain path data.

Marks come from two open collections of single-colour SVGs (24 px grid, fill = currentColor):
  lobe    https://github.com/lobehub/lobe-icons  (npm @lobehub/icons-static-svg, MIT)  AI products and tools
  simple  https://github.com/simple-icons/simple-icons  (npm simple-icons, CC0)          everything else
The marks stay the property of their owners; they are only used to tell the agents apart (see NOTICE.md).

An SVG is converted only when it is trivially representable: <path> elements, one fill rule, no transforms,
gradients, masks, opacity or other shapes. Anything else is rejected and the agent keeps its monogram tile.

  mkdir lobe simple && tar xzf icons-static-svg-X.tgz -C lobe && tar xzf simple-icons-X.tgz -C simple
  python3 -I tools/gen-provider-icons.py LOBE/package/icons SIMPLE/package/icons lobe-X simple-icons-Y > data/provider-icons.json
"""
import json
import re
import sys
import xml.etree.ElementTree as ET

# provider id -> (collection, file name without .svg). Only marks of the product itself, never a stand-in:
# a vendor's logo is used only when the agent has no mark of its own and is the vendor's one coding product.
MAP = {
    "claude-code": ("lobe", "claude"),
    "codex": ("lobe", "codex"),
    "gemini-cli": ("lobe", "geminicli"),
    "antigravity": ("lobe", "antigravity"),
    "cursor": ("lobe", "cursor"),
    "github-copilot": ("lobe", "githubcopilot"),
    "opencode": ("simple", "opencode"),
    "windsurf": ("lobe", "windsurf"),
    "zed": ("simple", "zedindustries"),
    "amp": ("lobe", "amp"),
    "goose": ("lobe", "goose"),
    "kiro-cli": ("lobe", "kiro"),
    "qwen-code": ("lobe", "qwen"),
    "cline": ("lobe", "cline"),
    "roo": ("lobe", "roocode"),
    "kilo": ("lobe", "kilocode"),
    "junie": ("lobe", "junie"),
    "openhands": ("lobe", "openhands"),
    "codebuddy": ("lobe", "codebuddy"),
    "command-code": ("lobe", "commandcode"),
    "devin": ("lobe", "devin"),
    "grok": ("lobe", "grok"),
    "hermes-agent": ("lobe", "hermesagent"),
    "kimi-code-cli": ("lobe", "kimi"),
    "mistral-vibe": ("lobe", "mistral"),
    "minimax-code": ("lobe", "minimax"),
    "openclaw": ("lobe", "openclaw"),
    "qoder": ("lobe", "qoder"),
    "qoder-cn": ("lobe", "qoder"),
    "trae": ("lobe", "trae"),
    "trae-cn": ("lobe", "trae"),
    "zencoder": ("lobe", "zencoder"),
    "warp": ("simple", "warp"),
    "replit": ("simple", "replit"),
}

ROOT_OK = {"fill", "fill-rule", "height", "width", "style", "viewBox", "xmlns", "role", "class", "aria-label"}
PATH_OK = {"d", "fill-rule", "clip-rule"}


def local(tag):
    return tag.rsplit("}", 1)[-1]


def norm(d):
    return re.sub(r"\s+", "", d.strip()).lower().replace(",", "")


def full_canvas(vb):
    w, h = (("%g" % vb[2]), ("%g" % vb[3]))
    return {"m00h%sv%sh0z" % (w, h), "m00h%sv%sh-%sz" % (w, h, w), "m00v%sh%sv0z" % (h, w)}


def convert(svg_text):
    root = ET.fromstring(svg_text)
    if local(root.tag) != "svg":
        raise ValueError("not an svg")
    for k in root.attrib:
        if local(k) not in ROOT_OK:
            raise ValueError("root attribute " + k)
    if root.get("fill", "currentColor") != "currentColor":
        raise ValueError("fill is not currentColor")
    vb = [float(x) for x in re.split(r"[ ,]+", root.get("viewBox", "").strip())]
    if len(vb) != 4 or vb[2] <= 0 or vb[3] <= 0:
        raise ValueError("bad viewBox")
    rule = root.get("fill-rule", "nonzero")
    paths = []
    full = full_canvas(vb)
    noop_clips = set()  # ids of clipPaths that clip to the whole canvas, i.e. do nothing
    clip_parts = set()
    for cp in (e for e in root.iter() if local(e.tag) == "clipPath"):
        clip_parts.update(id(e) for e in cp.iter())
        kids = list(cp)
        if len(kids) == 1 and local(kids[0].tag) == "path" and norm(kids[0].get("d", "")) in full:
            noop_clips.add(cp.get("id"))
    for el in root.iter():
        name = local(el.tag)
        if el is root or name in ("title", "defs") or id(el) in clip_parts:
            continue
        if name == "g":
            ref = re.fullmatch(r"url\(#(.+)\)", el.get("clip-path", ""))
            if set(el.attrib) != {"clip-path"} or not ref or ref.group(1) not in noop_clips:
                raise ValueError("element <g> that is not a no-op clip group")
            continue
        if name != "path":
            raise ValueError("element <%s>" % name)
        for k in el.attrib:
            if k not in PATH_OK:
                raise ValueError("path attribute " + k)
        d = el.get("d", "").strip()
        if not d:
            raise ValueError("empty path")
        paths.append((d, el.get("fill-rule", rule) == "evenodd"))
    if not paths:
        raise ValueError("no path")
    if len({eo for _, eo in paths}) > 1:
        raise ValueError("mixed fill rules")
    return {"vb": [int(v) if v == int(v) else v for v in vb],
            "paths": [{"d": d, "eo": eo} for d, eo in paths]}


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    dirs = {"lobe": sys.argv[1], "simple": sys.argv[2]}
    names = {"lobe": sys.argv[3], "simple": sys.argv[4]}
    icons, rejected = {}, []
    for pid, (coll, slug) in MAP.items():
        try:
            with open("%s/%s.svg" % (dirs[coll], slug), encoding="utf-8") as f:
                icon = convert(f.read())
        except (OSError, ValueError, ET.ParseError) as e:
            rejected.append("%s (%s:%s): %s" % (pid, coll, slug, e))
            continue
        icon["src"] = "%s:%s" % (coll, slug)
        icons[pid] = icon
    for r in rejected:
        print("rejected", r, file=sys.stderr)
    print("%d icons, %d rejected" % (len(icons), len(rejected)), file=sys.stderr)
    out = {
        "format": "skillhub-provider-icons",
        "version": 1,
        "source": "%s (MIT) and %s (CC0). Brand marks belong to their owners; used only to identify each agent." % (names["lobe"], names["simple"]),
        "icons": icons,
    }
    json.dump(out, sys.stdout, indent=1, sort_keys=True)
    print()


if __name__ == "__main__":
    main()
