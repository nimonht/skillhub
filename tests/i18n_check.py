#!/usr/bin/env python3
"""The window's translations are complete, and no English slips past them.

usage: python3 tests/i18n_check.py            check (CI)
       python3 tests/i18n_check.py --missing  print the texts a catalog lacks, as JSON to fill in

Every qsTr("...") in qml/ and tr("...") in src/ (the window's C++) is a key in i18n/<code>.json, which maps the English text to
its translation (src/gui/translator.cpp). Checked here, with no Qt needed:
  - every catalog has every text, with a non-empty translation, and no text that is gone from the code;
  - a translation keeps the same %1, %2 ... as its English, since .arg() fills them in;
  - qsTr() and tr() take a plain string, never a sum: lookups go by the exact text;
  - QML shows no English outside qsTr(). A line that means to (a name: "Skill Hub", "English") ends in // i18n-ok.
"""
import glob
import json
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
QML = sorted(glob.glob(os.path.join(ROOT, "qml", "**", "*.qml"), recursive=True))
CPP = [os.path.join(ROOT, "src", f) for f in ("app.cpp", "main.cpp")] + sorted(glob.glob(os.path.join(ROOT, "src", "gui", "**", "*.cpp"), recursive=True))
CATALOGS = sorted(glob.glob(os.path.join(ROOT, "i18n", "*.json")))

problems = []


def where(path, text, offset):
    return "%s:%d" % (os.path.relpath(path, ROOT), text.count("\n", 0, offset) + 1)


def unescape(body, js):
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        n = body[i + 1]
        i += 2
        if n == "n":
            out.append("\n")
        elif n == "t":
            out.append("\t")
        elif n == "u" and js:
            out.append(chr(int(body[i:i + 4], 16)))
            i += 4
        else:
            out.append(n)
    return "".join(out)


LITERAL = r'"((?:[^"\\\n]|\\.)*)"'


def literals_after(text, start, js):
    """The string literal(s) that start at `start` (C++ joins neighbours), and where they end; None if it is no literal."""
    parts, i = [], start
    while True:
        m = re.compile(r"\s*" + LITERAL).match(text, i)
        if not m:
            break
        parts.append(unescape(m.group(1), js))
        i = m.end()
        if js:
            break
    if not parts:
        return None, start
    return "".join(parts), i


def collect():
    keys = {}
    for path in QML:
        text = open(path, encoding="utf-8").read()
        for m in re.finditer(r"\bqsTr\(", text):
            s, end = literals_after(text, m.end(), True)
            if s is None or not re.match(r"\s*[,)]", text[end:]):
                problems.append("%s: qsTr() takes one plain string" % where(path, text, m.start()))
                continue
            comma = re.match(r"\s*,", text[end:])
            if comma:   # qsTr("Clear", "selection"): another meaning of the same English, "selection|Clear" in a catalog
                meaning, _ = literals_after(text, end + comma.end(), True)
                if meaning:
                    s = meaning + "|" + s
            keys.setdefault(s, where(path, text, m.start()))
    for path in CPP:
        text = open(path, encoding="utf-8").read()
        for m in re.finditer(r"(?<![\w.])tr\((?!\s*const\b)", text):   # not the definition of a local tr()
            s, end = literals_after(text, m.end(), False)
            if s is None or not re.match(r"\s*[,)]", text[end:]):
                problems.append("%s: tr() takes one plain string" % where(path, text, m.start()))
                continue
            keys.setdefault(s, where(path, text, m.start()))
    return keys


# QML: a literal that reads as words, outside qsTr(), is English the user would see
NOT_TEXT_KEYS = r"(?<![.\w])(?:value|id|key|icon|iconName|name|kind|place|wait|l|k|link|tone|source|section|mode|state|level|objectName|cursorShape|shape|role|accent|scope|sequence|sequences)"


TEXT_KEYS = r"(?:(?<![.\w])(?:text|t|label|title|note|hint|placeholderText|tip|shown)|Accessible\.name|Accessible\.description)"


def untranslated():
    for path in QML:
        lines = open(path, encoding="utf-8").read().split("\n")
        for n, line in enumerate(lines, 1):
            code = line.split("//")[0] if "//" in line and not re.search(r'"[^"]*//[^"]*"', line) else line
            if line.rstrip().endswith("// i18n-ok") or code.lstrip().startswith(("import ", "//", "*")):
                continue
            for m in re.finditer(LITERAL, code):
                s = m.group(1)
                before = code[:m.start()]
                if re.search(r"qsTr\(\s*$", before) or re.search(r"qsTr\([^()]*$", before):
                    continue
                if re.search(r"(===|!==|==|!=|case)\s*$", before) or re.search(NOT_TEXT_KEYS + r"\s*:\s*(\[\s*)?$", before):
                    continue
                if re.search(r"(setting|setSetting|setLook|look|hasOwnProperty|includes|indexOf|startsWith|endsWith|split|join|replace|arg|platform|layoutState|saveLayout|Qt\.\w+|\.\w+)\(\s*([^()]*,\s*)?$", before):
                    continue
                if not re.search(r"[A-Za-z]{2}", s) or s.startswith(("qrc:", "#", "http", "file:", "image:", "~/", "/")):
                    continue
                if re.fullmatch(r"[MmLlHhVvCcSsQqTtAaZz0-9 .,\-]+", s):
                    continue   # icon path data
                piece = (s != s.strip() and re.search(r"[a-z]{2}", s)) or re.fullmatch(r"[A-Z]{3,}", s)   # " skills", "HIGH": words too
                shown = re.search(r"\b" + TEXT_KEYS + r"\s*:\s*$", before) or re.search(r"\b" + TEXT_KEYS + r"\s*:[^;{}]*[?:]\s*$", before)   # or a branch of it
                if not shown and not piece and not (" " in s.strip() or re.match(r"[A-Z][a-z]", s)):
                    continue   # one lowercase word: a key, an icon, a mode, unless it is given to a text
                problems.append("%s:%d: English outside qsTr(): \"%s\"" % (os.path.relpath(path, ROOT), n, s))


def placeholders(s):
    return sorted(set(re.findall(r"%\d+", s)))


def main():
    keys = collect()
    untranslated()
    if not CATALOGS:
        problems.append("no catalogs in i18n/")
    for path in CATALOGS:
        name = os.path.relpath(path, ROOT)
        catalog = json.load(open(path, encoding="utf-8"))
        texts = {k: v for k, v in catalog.items() if not k.startswith("@")}
        missing = [k for k in keys if k not in texts]
        if "--missing" in sys.argv:
            print(json.dumps({k: "" for k in missing}, ensure_ascii=False, indent=2))
            continue
        for k in missing:
            problems.append("%s: no translation of \"%s\" (%s)" % (name, k, keys[k]))
        for k, v in texts.items():
            if k not in keys:
                problems.append("%s: \"%s\" is no longer in the code" % (name, k))
            elif not isinstance(v, str) or not v.strip():
                problems.append("%s: \"%s\" has an empty translation" % (name, k))
            elif placeholders(k) != placeholders(v):
                problems.append("%s: \"%s\" uses %s, its translation %s" % (name, k, placeholders(k), placeholders(v)))
    if "--missing" in sys.argv:
        return 0
    for p in problems:
        print(p, file=sys.stderr)
    if problems:
        print("%d problem(s) with the translations" % len(problems), file=sys.stderr)
        return 1
    print("i18n: %d texts, each translated in %s" % (len(keys), ", ".join(os.path.basename(c)[:-5] for c in CATALOGS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
