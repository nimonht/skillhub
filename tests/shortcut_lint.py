#!/usr/bin/env python3
"""Every shortcut of the window is either one the terminal lets through, or one the terminal owns when it has the keyboard.

A focused terminal takes every key that is not in its list (src/gui/terminal/vterm_item.cpp, windowKeeps), so "/" and Escape and
Ctrl+R reach the shell. A new Shortcut in qml/Main.qml therefore needs a decision, made here: leave it to the terminal (it stops working
while the terminal has the keyboard), or add its chord to windowKeeps. Exits 1 on a shortcut nobody has decided about, and on a chord
that the terminal keeps for a shortcut that no longer exists.

usage: python3 tests/shortcut_lint.py"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
main = (ROOT / "qml" / "Main.qml").read_text()
item = (ROOT / "src" / "gui" / "terminal" / "vterm_item.cpp").read_text()

# the window's shortcuts: sequence: "X" or sequences: ["X", "Y"]
shortcuts = set()
for m in re.finditer(r'Shortcut\s*\{\s*(sequences?):\s*(\[[^\]]*\]|"[^"]*")', main):
    shortcuts.update(re.findall(r'"([^"]+)"', m[2]))

# the chords the terminal lets through
keys = {"QuoteLeft": "`", "Comma": ",", "Left": "Left", "Right": "Right", "Up": "Up", "Down": "Down"}
kept = set()
body = item[item.index("static bool windowKeeps"):item.index("bool VTermItem::event")]
for m in re.finditer(r"QKeySequence\(([^()]*)\)", body):
    parts = [p.strip() for p in m[1].split("|")]
    mods = [{"Qt::CTRL": "Ctrl", "Qt::ALT": "Alt", "Qt::SHIFT": "Shift"}[p] for p in parts if p in ("Qt::CTRL", "Qt::ALT", "Qt::SHIFT")]
    key = [p for p in parts if p.startswith("Qt::Key_")]
    if len(key) == 1:
        name = key[0][len("Qt::Key_"):]
        kept.add("+".join(mods + [keys.get(name, name)]))

# decided: these stop working while the terminal has the keyboard, because the shell wants them (or, Ctrl+Q on other systems than macOS,
# because a program in the terminal may)
TERMINAL_OWNS = {"/", "Escape", "F1", "F5", "Ctrl+F", "Ctrl+N", "Ctrl+U", "Ctrl+R", "Ctrl+J", "Ctrl+Q"}

bad = 0
for s in sorted(shortcuts - kept - TERMINAL_OWNS):
    print(f"undecided: Shortcut {s!r} in qml/Main.qml is neither kept by the terminal (windowKeeps) nor listed in TERMINAL_OWNS")
    bad += 1
for s in sorted(kept - shortcuts):
    print(f"stale: windowKeeps lets {s!r} through, and qml/Main.qml has no such Shortcut")
    bad += 1
for s in sorted(TERMINAL_OWNS - shortcuts):
    print(f"stale: TERMINAL_OWNS lists {s!r}, and qml/Main.qml has no such Shortcut")
    bad += 1
print(f"{len(shortcuts)} shortcuts: {len(kept)} kept for the window, {len(shortcuts & TERMINAL_OWNS)} left to the terminal" if not bad else f"{bad} problem(s)")
sys.exit(1 if bad else 0)
