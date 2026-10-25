#!/usr/bin/env python3
"""Differential test: run the same scenarios against Skill Hub 1.0.1 and the build under test, and compare what each
printed, its exit codes and the files it left behind. This is the parity gate for the 2.0 rewrite of the core in C.

usage: python3 tests/diff_vs_1_0.py OLD_BINARY NEW_BINARY [--keep]

Everything is offline: each binary gets its own throw-away home with identical content, and the "upstream" repository is
a local one that the cache already holds. Differences that are intended are listed in KNOWN_DIFFERENCES with the reason.
"""
import base64
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

OLD, NEW = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
KEEP = "--keep" in sys.argv
WIN = os.name == "nt"

# (command, reason): output that may differ on purpose
KNOWN_DIFFERENCES = {
    "--version": "the version number",
}


def fail(msg):
    print("FAIL:", msg, file=sys.stderr)
    sys.exit(1)


def git(cwd, *args):
    env = dict(os.environ, GIT_AUTHOR_DATE="2026-01-01T00:00:00Z", GIT_COMMITTER_DATE="2026-01-01T00:00:00Z")
    p = subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.com", "-c", "core.autocrlf=false",
                        "-c", "init.defaultBranch=main", *args], cwd=cwd, capture_output=True, text=True, timeout=60, env=env)
    if p.returncode != 0:
        fail("git %s: %s" % (" ".join(args), p.stderr))
    return p.stdout.strip()


def write(path, text, newline="\n"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline=newline) as f:
        f.write(text)


def skill(path, name, description="test skill", body="body"):
    write(os.path.join(path, "SKILL.md"), "---\nname: %s\ndescription: %s\n---\n\n%s\n" % (name, description, body))


def build_upstream(root, label):
    """A local 'GitHub' repository with two skills, one per scenario so both start from identical content."""
    up = os.path.join(root, label + "-upstream")
    skill(os.path.join(up, "skills", "one"), "one", "first upstream skill", "version 1")
    write(os.path.join(up, "skills", "one", "sub", "file.txt"), "nested\n")
    write(os.path.join(up, "skills", "one", "run.sh"), "#!/bin/sh\necho one\n")
    os.chmod(os.path.join(up, "skills", "one", "run.sh"), 0o755)
    skill(os.path.join(up, "skills", "two"), "two", "second upstream skill")
    skill(os.path.join(up, "tests", "fixture"), "fixture", "never offered: inside tests/")
    git(up, "init", "-q")
    git(up, "add", "-A")
    git(up, "update-index", "--chmod=+x", "skills/one/run.sh")
    git(up, "commit", "-q", "-m", "first")
    return up


def make_home(root, label, upstream):
    home = os.path.join(root, label)
    os.makedirs(home)
    env = dict(os.environ, HOME=home, USERPROFILE=home, SKILLHUB_HOME=home,
               XDG_CONFIG_HOME=home + "/.config", XDG_CACHE_HOME=home + "/.cache",
               XDG_STATE_HOME=home + "/.local/state", XDG_DATA_HOME=home + "/.local/share",
               SKILLHUB_CACHE=home + "/cache/skillhub", SKILLHUB_CONFIG=home + "/config/skillhub",
               SKILLHUB_STATE=home + "/state/skillhub", LC_ALL="C.UTF-8")
    stubs = os.path.join(home, "provbin")
    os.makedirs(stubs)
    for name in ("codex", "agy"):
        stub, body = (name + ".cmd", "@exit /b 0\r\n") if WIN else (name, "#!/bin/sh\nexit 0\n")
        write(os.path.join(stubs, stub), body, newline="")
        os.chmod(os.path.join(stubs, stub), 0o755)
    env["SKILLHUB_BIN_PATH"] = stubs

    central = os.path.join(home, ".agents", "skills")
    os.makedirs(os.path.join(home, ".claude"))
    os.makedirs(os.path.join(home, ".gemini"))
    write(os.path.join(home, ".gemini", "settings.json"), "{}")
    skill(os.path.join(central, "alpha"), "alpha")
    skill(os.path.join(home, ".claude", "skills", "beta"), "beta", "stray in claude")
    skill(os.path.join(home, ".codex", "skills", "gamma"), "gamma", "stray in codex")
    skill(os.path.join(home, ".codex", "skills", "alpha"), "alpha")                        # identical duplicate
    skill(os.path.join(home, ".gemini", "skills", "alpha"), "alpha", "different text")     # conflicting copy
    skill(os.path.join(home, ".codex", "skills", ".system", "builtin"), "builtin")          # tool-owned
    # front matter the parser must read exactly like 1.0.1 did
    write(os.path.join(central, "blocky", "SKILL.md"),
          "---\nname: blocky\ndescription: |\n  first line\n    indented   more\n  last line\nmetadata:\n  nested: map\n---\nbody\n")
    write(os.path.join(central, "folded", "SKILL.md"), "---\nname: folded\ndescription: >-\n  folded\n  text\n---\n")
    write(os.path.join(central, "quoted", "SKILL.md"), "---\nname: quoted\ndescription: \"a: b, 'c'\"\n---\n")
    write(os.path.join(central, "crlf", "SKILL.md"), "---\r\nname: crlf\r\ndescription: windows line ends\r\n---\r\nbody\r\n", newline="")
    write(os.path.join(central, "unclosed", "SKILL.md"), "---\nname: unclosed\ndescription: never closed\n")
    write(os.path.join(central, "plain", "SKILL.md"), "no front matter at all\n")
    write(os.path.join(central, "bom", "SKILL.md"), "﻿---\nname: bom\ndescription: starts with a byte order mark\n---\n")
    write(os.path.join(central, "spaces", "SKILL.md"), "---\nname: spaces\ndescription: non-breaking spaces around \t\n---\n")
    write(os.path.join(central, "unicode", "SKILL.md"), "---\nname: unicode\ndescription: Ünïcödé — 日本語 ✓ emoji 🧩\n---\n")
    skill(os.path.join(central, "café-skill"), "café-skill", "a non-ASCII folder name")

    # the cache already holds a clone of the "upstream" repository, as in tests/portable_smoke.py
    cached = os.path.join(env["SKILLHUB_CACHE"], "repos", "acme__skills")
    os.makedirs(os.path.dirname(cached))
    git(home, "clone", "-q", "--depth", "1", ("file://" + upstream.replace("\\", "/")) if WIN else upstream, cached)
    return home, env


def hostile_export(path):
    b = lambda data: base64.b64encode(data).decode()
    skills = [
        {"name": "b64-plain", "local": True, "files": [{"path": "SKILL.md", "data": b(b"---\nname: b64-plain\ndescription: d\n---\n")}]},
        # lenient base64: missing padding, line breaks, characters outside the alphabet
        {"name": "b64-loose", "local": True, "files": [
            {"path": "SKILL.md", "data": b(b"---\nname: b64-loose\ndescription: d\n---\n")},
            {"path": "nopad.bin", "data": b(b"ab").rstrip("=")},
            {"path": "wrapped.bin", "data": "\n".join(b(bytes(range(64)))[i:i + 10] for i in range(0, 88, 10))},
            {"path": "urlsafe.bin", "data": base64.urlsafe_b64encode(bytes([251, 255, 254, 62, 63])).decode()},
            {"path": "junk.bin", "data": "QUJD*&^%REVG"},
        ]},
        {"name": "../evil", "local": True, "files": [{"path": "SKILL.md", "data": b(b"x")}]},
        {"name": "climb", "local": True, "files": [{"path": "SKILL.md", "data": b(b"x")}, {"path": "../../escaped.txt", "data": b(b"x")}]},
        {"name": "gitdir", "local": True, "files": [{"path": "SKILL.md", "data": b(b"x")}, {"path": ".git/hooks/pre-commit", "data": b(b"x")}]},
        {"name": "badsource", "source": "evil.example.com/x/y", "path": "skills/badsource/SKILL.md"},
    ]
    write(path, json.dumps({"format": "skillhub-export", "version": 1, "skills": skills}))


# ---- normalising what we compare ----------------------------------------------------------------------------------------

STAMP = re.compile(r"\d{8}-\d{6}(-\d+)?")
VERSION = re.compile(r"skillhub \d+\.\d+\.\d+(-[A-Za-z0-9.]+)?")
ISO = re.compile(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?Z?")


def norm_text(text, home, upstream):
    text = text.replace("\r\n", "\n").replace(upstream, "<UPSTREAM>").replace(home, "<HOME>")
    return VERSION.sub("skillhub <VERSION>", ISO.sub("<TIME>", STAMP.sub("<STAMP>", text)))


def norm_json(value, home, upstream):
    if isinstance(value, dict):
        out = {}
        for k, v in value.items():
            if k in ("installedAt", "updatedAt", "createdAt", "createdBy"):
                v = "<MASKED>"
            out[k] = norm_json(v, home, upstream)
        return out
    if isinstance(value, list):
        return [norm_json(v, home, upstream) for v in value]
    if isinstance(value, str):
        # 1.0.1 could cut a name in the middle of a surrogate pair (QString::left); 2.0 keeps whole characters
        return norm_text(re.sub("[\ud800-\udfff]", "", value), home, upstream)
    return value


SKIP_DIRS = {"cache", "provbin", ".cache", ".local"}


def snapshot(home, upstream):
    """Every entry below home (except the git cache and stubs): type, link target, content."""
    out = {}
    for dirpath, dirnames, filenames in os.walk(home):
        rel_dir = os.path.relpath(dirpath, home).replace("\\", "/")
        if rel_dir == ".":
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in sorted(dirnames + filenames):
            full = os.path.join(dirpath, name)
            rel = STAMP.sub("<STAMP>", os.path.normpath(os.path.join(rel_dir, name)).replace("\\", "/"))
            if os.path.islink(full):
                out[rel] = "link -> " + norm_text(os.readlink(full), home, upstream)
            elif os.path.isdir(full):
                out[rel] = "dir"
            else:
                data = open(full, "rb").read()
                if name.endswith(".json"):
                    try:
                        out[rel] = "json " + json.dumps(norm_json(json.loads(data), home, upstream), sort_keys=True)
                        continue
                    except ValueError:
                        pass
                mode = "x" if (not WIN and os.access(full, os.X_OK)) else "-"
                out[rel] = "file %s %s" % (mode, hashlib.sha256(data).hexdigest()[:16])
        dirnames[:] = [d for d in dirnames if not os.path.islink(os.path.join(dirpath, d))]
    return out


def run(binary, env, args, home, upstream):
    p = subprocess.run([binary, *args], env=env, capture_output=True, text=True, encoding="utf-8", errors="replace",
                       timeout=180, stdin=subprocess.DEVNULL)
    out = norm_text(p.stdout, home, upstream)
    try:  # JSON output is compared by meaning: key order and indentation are not part of the contract
        out = "json " + json.dumps(norm_json(json.loads(p.stdout), home, upstream), sort_keys=True)
    except ValueError:
        pass
    return {"exit": p.returncode, "out": out, "err": norm_text(p.stderr, home, upstream)}


# ---- the scenario ---------------------------------------------------------------------------------------------------------

def scenario(binary, label, root):
    upstream = build_upstream(root, label)
    home, env = make_home(root, label, upstream)
    results = []

    def step(*args, mutate=False):
        r = run(binary, env, list(args), home, upstream)
        r["cmd"] = " ".join(args)
        if mutate:
            r["files"] = snapshot(home, upstream)
        results.append(r)

    step("--version")
    step("list")
    step("list", "--json")
    step("doctor")
    step("providers")
    step("providers", "--json")
    step("providers", "--all")
    step("adopt", mutate=True)
    step("adopt")
    step("doctor")
    step("list", "--json")
    step("providers", "add", "Café Agent", home + "/.cafe/skills", mutate=True)
    step("providers", "add", "A" * 39 + "\U0001F9E9", home + "/.long/skills", mutate=True)   # cut inside a surrogate pair
    step("providers", "add", "Bad", home)
    step("providers", "remove", "custom-café-agent", mutate=True)
    step("add", "acme/skills")
    step("add", "acme/skills", "--all", mutate=True)
    step("check")
    write(os.path.join(upstream, "skills", "one", "SKILL.md"), "---\nname: one\ndescription: first upstream skill\n---\n\nversion 2\n")
    write(os.path.join(upstream, "skills", "two", "SKILL.md"), "---\nname: two\ndescription: second upstream skill\n---\n\nversion 2\n")
    git(upstream, "commit", "-q", "-am", "second")
    step("check", "one")             # 1.0.1 checks every skill here and counts them all in the exit code
    step("update", "one", mutate=True)
    step("update", "--all", mutate=True)
    step("track", "alpha", "acme/skills")
    step("remove", "beta", mutate=True)
    step("export", home + "/export.json", mutate=True)
    hostile_export(home + "/hostile.json")
    step("import", home + "/hostile.json", "--yes", mutate=True)
    step("import", home + "/hostile.json")
    step("frobnicate")
    step("update")
    return results


def main():
    root = os.path.realpath(tempfile.mkdtemp(prefix="skillhub-diff-"))
    try:
        old = scenario(OLD, "old", root)
        new = scenario(NEW, "new", root)
        bad = 0
        for a, b in zip(old, new):
            cmd = a["cmd"]
            for key in ("exit", "out", "err", "files"):
                if a.get(key) == b.get(key):
                    continue
                if cmd in KNOWN_DIFFERENCES:
                    print("known difference in `%s` (%s): %s" % (cmd, key, KNOWN_DIFFERENCES[cmd]))
                    continue
                bad += 1
                print("DIFFERENT `skillhub %s` %s" % (cmd, key))
                if key == "files":
                    fa, fb = a["files"], b["files"]
                    for path in sorted(set(fa) | set(fb)):
                        if fa.get(path) != fb.get(path):
                            print("   %s\n      1.0.1: %s\n      new:   %s" % (path, fa.get(path), fb.get(path)))
                else:
                    print("   1.0.1: %r\n   new:   %r" % (a.get(key), b.get(key)))
        if bad:
            fail("%d difference(s) between 1.0.1 and the new build" % bad)
        print("differential test passed (%d commands)" % len(old))
    finally:
        if KEEP:
            print("kept", root)
        else:
            shutil.rmtree(root, ignore_errors=True)


main()
