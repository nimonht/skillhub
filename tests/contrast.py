#!/usr/bin/env python3
"""WCAG contrast gate for the design tokens in qml/Theme.qml (dark and light).
Text pairs must reach 4.5:1, graphics and control boundaries 3:1. Exits 1 on any miss.

usage: python3 tests/contrast.py [PATH/TO/skillhub]
With the program, the accent roles derived from 72 hues at 3 chroma levels are checked too, in both modes."""
import json, re, subprocess, sys, pathlib

src = (pathlib.Path(__file__).resolve().parent.parent / "qml" / "Theme.qml").read_text()
tok = {m[1].removesuffix("Default"): (m[2], m[3]) for m in re.finditer(r'readonly property color (\w+): dark \? "(#[0-9A-Fa-f]{6})" : "(#[0-9A-Fa-f]{6})"', src)}

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
checks.append(("text2", "trackOff", GRAPHIC))   # switch knob on its off track
checks.append(("text3", "field", TEXT))       # placeholder text on the input fill
checks.append(("text", "field", TEXT))
checks += [(fg, "bg3", TEXT) for fg in ("markText", "notifyText", "dangerText")]   # the toast

bad = 0
for mode, name in ((0, "dark"), (1, "light")):
    for fg, bg, need in checks:
        if fg not in tok or bg not in tok:
            print(f"missing token {fg}/{bg}"); bad += 1; continue
        r = ratio(tok[fg][mode], tok[bg][mode])
        if r < need:
            print(f"FAIL {name:5s} {fg:10s} on {bg:9s} {r:5.2f} < {need}"); bad += 1

# Blur and Liquid Glass: panels at their alpha floor over pure black and pure white, with the text they use then
import colorsys
FLOOR = {0: float(re.search(r"alphaFloor: dark \? ([0-9.]+)", src)[1]), 1: float(re.search(r"alphaFloor: dark \? [0-9.]+ : ([0-9.]+)", src)[1])}
FLOOR_GLASS = {0: float(re.search(r"glassFloor: dark \? ([0-9.]+)", src)[1]), 1: float(re.search(r"glassFloor: dark \? [0-9.]+ : ([0-9.]+)", src)[1])}
BACKDROP_HI = float(re.search(r"glassBackdropHi: ([0-9.]+)", src)[1])   # the system's glass in the dark theme: never lighter than this grey
BACKDROP_LO = float(re.search(r"glassBackdropLo: ([0-9.]+)", src)[1])   # in the light theme: never darker
TINT_ALPHA = float(re.search(r"glassTintAlpha: ([0-9.]+)", src)[1])     # a surface that floats over the program's own content
DARKER = float(re.search(r"Qt\.darker\(c, ([0-9.]+)\)", src)[1])

def hexof(c):
    return "#%02X%02X%02X" % tuple(round(v * 255) for v in c)

def rgb(h):
    return [int(h[i:i + 2], 16) / 255 for i in (1, 3, 5)]

def strengthened(h, m):
    if m == 0:
        return h
    hh, s, v = colorsys.rgb_to_hsv(*rgb(h))
    return hexof(colorsys.hsv_to_rgb(hh, s, v / DARKER))

BLACK_WHITE = ((0, 0, 0), (1, 1, 1))
def native_backdrops(m):   # the system's glass, whose appearance is the theme's: bounded on the side the text is not
    return ((0, 0, 0), (BACKDROP_HI,) * 3) if m == 0 else ((BACKDROP_LO,) * 3, (1, 1, 1))

def glass_checks(t, m, label, floor=None, overs=BLACK_WHITE, what="its floor"):
    floor = FLOOR[m] if floor is None else floor
    miss = 0
    for bg in ("bg1", "bg2"):
        for over in overs:
            base = hexof([floor * x + (1 - floor) * o for x, o in zip(rgb(t[bg]), over)])
            for fg in ("text", "text2", "markText", "notifyText", "dangerText", "gainText"):
                colour = t[fg] if fg in ("text", "text2") else strengthened(t[fg], m)
                r = ratio(colour, base)
                if r < TEXT:
                    print(f"FAIL {label} {fg:10s} on {bg} at {what} over {over}: {r:5.2f} < {TEXT}")
                    miss += 1
    return miss

def shader_glass_checks(t, m, label):   # a toast, a menu, a tooltip: tinted glass over the program's own surfaces, with a lit rim on top
    miss = 0
    for behind in ("bg0", "bg1", "bg2", "bg4"):
        base = [TINT_ALPHA * x + (1 - TINT_ALPHA) * y for x, y in zip(rgb(t["bg3"]), rgb(t[behind]))]
        base = hexof([min(1, v + 0.04) for v in base])   # the glow of the rim, as far in as text sits (the bevel is 18 px, text at least 12 from the edge)
        for fg in ("text", "text2", "markText", "notifyText", "dangerText"):
            colour = t[fg] if fg in ("text", "text2") else strengthened(t[fg], m)
            r = ratio(colour, base)
            if r < TEXT:
                print(f"FAIL {label} {fg:10s} on shader glass over {behind}: {r:5.2f} < {TEXT}")
                miss += 1
    return miss

for mode, name in ((0, "dark"), (1, "light")):
    t = {k: v[mode] for k, v in tok.items()}
    bad += glass_checks(t, mode, name)
    bad += glass_checks(t, mode, name, FLOOR_GLASS[mode], native_backdrops(mode), "the system glass floor")
    bad += shader_glass_checks(t, mode, name)

def hsl(h, s, l):
    c = (1 - abs(2 * l - 1)) * s
    x = c * (1 - abs((h / 60) % 2 - 1))
    r, g, b = [(c, x, 0), (x, c, 0), (0, c, x), (0, x, c), (x, 0, c), (c, 0, x)][int(h // 60) % 6]
    m = l - c / 2
    return "#%02X%02X%02X" % tuple(round((v + m) * 255) for v in (r, g, b))

if len(sys.argv) > 1:
    seeds = [hsl(h * 5, s, 0.55) for h in range(72) for s in (0.35, 0.65, 0.95)]
    out = subprocess.run([sys.argv[1], "palette", "--seed", ",".join(seeds)], capture_output=True, text=True, check=True).stdout
    roles = json.loads(out)
    for p in roles:
        m = 0 if p["mode"] == "dark" else 1
        t = {k: v[m] for k, v in tok.items()}
        t.update({"action": p["action"], "actionInk": p["actionInk"], "mark": p["mark"], "markText": p["markText"], "markLow": p["markLow"],
                  "focus": p["focus"], "bg4": p["selected"]})
        for fg, bg, need in checks:
            r = ratio(t[fg], t[bg])
            if r < need:
                print(f"FAIL {p['seed']} {p['mode']:5s} {fg:10s} on {bg:9s} {r:5.2f} < {need}"); bad += 1
        bad += glass_checks(t, m, p["seed"] + " " + p["mode"])
        bad += glass_checks(t, m, p["seed"] + " " + p["mode"], FLOOR_GLASS[m], native_backdrops(m), "the system glass floor")
        bad += shader_glass_checks(t, m, p["seed"] + " " + p["mode"])
        for fg in ("text2", "markText"):   # tinted surfaces; text3 gives way to text2 there
            for bg in ("bg1", "bg2"):
                tinted = hexof([0.97 * a + 0.03 * b for a, b in zip(rgb(t[bg]), rgb(t["mark"]))])
                if ratio(t[fg], tinted) < TEXT:
                    print(f"FAIL {p['seed']} {p['mode']:5s} {fg} on tinted {bg}: {ratio(t[fg], tinted):5.2f}"); bad += 1
    print(f"{len(roles)} derived palettes checked")
print("contrast gate passed" if not bad else f"{bad} contrast failure(s)")
sys.exit(1 if bad else 0)
