#!/usr/bin/env python3
"""WCAG contrast gate for the design tokens in qml/Theme.qml (dark and light).
Text pairs must reach 4.5:1, graphics and control boundaries 3:1. Exits 1 on any miss."""
import re, sys, pathlib

src = (pathlib.Path(__file__).resolve().parent.parent / "qml" / "Theme.qml").read_text()
tok = {m[1]: (m[2], m[3]) for m in re.finditer(r'readonly property color (\w+): dark \? "(#[0-9A-Fa-f]{6})" : "(#[0-9A-Fa-f]{6})"', src)}

def lum(h):
    r, g, b = [int(h[i:i + 2], 16) / 255 for i in (1, 3, 5)]
    f = lambda c: c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    return 0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b)

def ratio(a, b):
    la, lb = sorted((lum(a), lum(b)), reverse=True)
    return (la + 0.05) / (lb + 0.05)

TEXT = 4.5
GRAPHIC = 3.0
checks = []
for fg in ("text", "text2", "text3"):
    for bg in ("bg1", "bg2", "bg3", "bg4"):
        checks.append((fg, bg, TEXT))
for fg, bgs in (("markText", ("bg1", "bg2", "bg4", "markLow")), ("notifyText", ("bg1", "bg2", "bg4", "notifyLow")),
                ("gainText", ("bg2", "gainLow")), ("dangerText", ("bg1", "bg2", "bg4", "dangerLow"))):
    checks += [(fg, bg, TEXT) for bg in bgs]
checks.append(("actionInk", "action", TEXT))
for fg in ("mark", "notify", "danger", "focus", "lineInput"):
    checks += [(fg, bg, GRAPHIC) for bg in ("bg2", "bg3")]
checks += [("mark", "bg4", GRAPHIC), ("focus", "bg4", GRAPHIC)]
checks.append(("inkText", "inkBg", TEXT))
checks.append(("text2", "trackOff", GRAPHIC))   # switch knob on its off track
checks.append(("text3", "field", TEXT))       # placeholder text on the input fill
checks.append(("text", "field", TEXT))
checks += [(fg, "inkBg", GRAPHIC) for fg in ("inkOk", "inkWarn", "inkDanger")]

bad = 0
for mode, name in ((0, "dark"), (1, "light")):
    for fg, bg, need in checks:
        if fg not in tok or bg not in tok:
            print(f"missing token {fg}/{bg}"); bad += 1; continue
        r = ratio(tok[fg][mode], tok[bg][mode])
        if r < need:
            print(f"FAIL {name:5s} {fg:10s} on {bg:9s} {r:5.2f} < {need}"); bad += 1
print("contrast gate passed" if not bad else f"{bad} contrast failure(s)")
sys.exit(1 if bad else 0)
