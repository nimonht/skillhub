#!/usr/bin/env python3
"""Install, check and update benchmark for Skill Hub, offline.

usage: python3 bench/bench.py BINARY [--repos N] [--skills M] [--changed PERCENT] [--label NAME] [--json FILE]

Builds N local git repositories with M skills each and maps them to github.com/bench/<repo> with a throw-away git
config (url.insteadOf), so the GitHub-only 1.0.1 and newer builds run the very same fixture. Then, in a fresh home with
Claude Code "installed" (so every skill also gets a link):

  install   `skillhub add bench/<repo> --all` for every repository
  check     `skillhub check`, nothing changed upstream
  check-10  `skillhub check` after PERCENT % of the skills changed upstream
  update    `skillhub update --all`

For each phase it records the wall time, the peak resident memory of the skillhub processes (sampled from /proc, Linux
only) and how many git processes were started (a `git` wrapper first in PATH counts them).
"""
import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ap = argparse.ArgumentParser()
ap.add_argument("binary")
ap.add_argument("--repos", type=int, default=25)
ap.add_argument("--skills", type=int, default=20)
ap.add_argument("--changed", type=float, default=10.0)
ap.add_argument("--label", default="")
ap.add_argument("--json", default="")
ap.add_argument("--keep", action="store_true")
args = ap.parse_args()
BIN = os.path.abspath(args.binary)
REAL_GIT = shutil.which("git")


def git(cwd, *a):
    subprocess.run([REAL_GIT, "-c", "user.name=b", "-c", "user.email=b@example.com", "-c", "init.defaultBranch=main", *a],
                   cwd=cwd, check=True, capture_output=True)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        f.write(text)


def build_fixture(root):
    repos = []
    for r in range(args.repos):
        name = "repo%04d" % r
        path = os.path.join(root, "fixture", name + ".git")   # a working repository whose folder name ends in .git
        for s in range(args.skills):
            d = os.path.join(path, "skills", "r%04ds%03d" % (r, s))
            write(os.path.join(d, "SKILL.md"), "---\nname: r%04ds%03d\ndescription: benchmark skill %d of repository %d\n---\n\n# Skill\n\nBody text.\n" % (r, s, s, r))
            write(os.path.join(d, "reference", "notes.md"), "notes %d %d\n" % (r, s) * 20)
        git(path, "init", "-q")
        git(path, "config", "uploadpack.allowFilter", "true")
        git(path, "add", "-A")
        git(path, "commit", "-q", "-m", "first")
        repos.append((name, path))
    return repos


def change_upstream(repos):
    """Edit PERCENT % of all skills, spread over the repositories, one commit per touched repository."""
    total = args.repos * args.skills
    want = max(1, int(total * args.changed / 100))
    touched = {}
    for i in range(want):
        r, s = i % args.repos, (i // args.repos) % args.skills
        touched.setdefault(r, []).append(s)
    for r, skills in touched.items():
        name, path = repos[r]
        for s in skills:
            with open(os.path.join(path, "skills", "r%04ds%03d" % (r, s), "SKILL.md"), "a", newline="\n") as f:
                f.write("\nChanged upstream.\n")
        git(path, "commit", "-q", "-am", "change")
    return want


class Peak:
    """Samples VmRSS of every process whose executable is BIN (the command line, and on 2.0 also the window's program
    it starts) while a phase runs."""

    def __init__(self):
        self.kb = 0
        self.stop = threading.Event()

    def _sample(self):
        names = {os.path.basename(BIN), "skillhub-gui"}
        while not self.stop.is_set():
            for pid in os.listdir("/proc"):
                if not pid.isdigit():
                    continue
                try:
                    with open("/proc/%s/comm" % pid) as f:
                        if f.read().strip() not in names:
                            continue
                    with open("/proc/%s/status" % pid) as f:
                        for line in f:
                            if line.startswith("VmHWM:"):
                                self.kb = max(self.kb, int(line.split()[1]))
                except OSError:
                    pass
            time.sleep(0.02)

    def __enter__(self):
        if sys.platform.startswith("linux"):
            self.t = threading.Thread(target=self._sample, daemon=True)
            self.t.start()
        return self

    def __exit__(self, *exc):
        self.stop.set()
        if sys.platform.startswith("linux"):
            self.t.join()


def main():
    root = os.path.realpath(tempfile.mkdtemp(prefix="skillhub-bench-"))
    try:
        t0 = time.perf_counter()
        repos = build_fixture(root)
        print("fixture: %d repositories x %d skills = %d skills (%.1f s)" % (args.repos, args.skills, args.repos * args.skills, time.perf_counter() - t0))

        home = os.path.join(root, "home")
        os.makedirs(os.path.join(home, ".claude"))
        wrap = os.path.join(root, "wrap")
        os.makedirs(wrap)
        log = os.path.join(root, "git-calls.log")
        with open(os.path.join(wrap, "git"), "w") as f:
            f.write('#!/bin/sh\necho x >> "%s"\nexec "%s" "$@"\n' % (log, REAL_GIT))
        os.chmod(os.path.join(wrap, "git"), 0o755)
        gitconfig = os.path.join(root, "gitconfig")
        with open(gitconfig, "w") as f:
            f.write('[url "file://%s/fixture/"]\n\tinsteadOf = https://github.com/bench/\n[protocol "file"]\n\tallow = always\n' % root)
        env = dict(os.environ, HOME=home, USERPROFILE=home, SKILLHUB_HOME=home, XDG_CONFIG_HOME=home + "/.config",
                   XDG_CACHE_HOME=home + "/.cache", XDG_STATE_HOME=home + "/.local/state", XDG_DATA_HOME=home + "/.local/share",
                   SKILLHUB_BIN_PATH=os.path.join(root, "none"), GIT_CONFIG_GLOBAL=gitconfig, GIT_CONFIG_NOSYSTEM="1",
                   PATH=wrap + os.pathsep + os.environ.get("PATH", ""))

        def phase(name, commands, expect=(0,)):
            if os.path.exists(log):
                os.remove(log)
            with Peak() as peak:
                t = time.perf_counter()
                for cmd in commands:
                    p = subprocess.run([BIN, *cmd], env=env, capture_output=True, text=True, stdin=subprocess.DEVNULL)
                    if p.returncode not in expect:
                        sys.exit("`skillhub %s` exited %d\n%s\n%s" % (" ".join(cmd), p.returncode, p.stdout[-2000:], p.stderr[-2000:]))
                wall = time.perf_counter() - t
            calls = sum(1 for _ in open(log)) if os.path.exists(log) else 0
            row = {"phase": name, "seconds": round(wall, 2), "peak_rss_mb": round(peak.kb / 1024, 1), "git_processes": calls}
            print("%-9s %9.2f s   peak %7.1f MB   %6d git processes" % (name, wall, peak.kb / 1024, calls))
            return row

        rows = [phase("install", [["add", "bench/" + name, "--all"] for name, _ in repos])]
        rows.append(phase("check", [["check"]]))
        changed = change_upstream(repos)
        rows.append(phase("check-%d" % args.changed, [["check"]], expect=(0, 10)))
        rows.append(phase("update", [["update", "--all"]]))
        listed = json.loads(subprocess.run([BIN, "list", "--json"], env=env, capture_output=True, text=True).stdout)
        if len(listed) != args.repos * args.skills:
            sys.exit("expected %d skills, found %d" % (args.repos * args.skills, len(listed)))

        result = {"label": args.label or os.path.basename(BIN), "binary": BIN,
                  "version": subprocess.run([BIN, "--version"], capture_output=True, text=True).stdout.strip(),
                  "repos": args.repos, "skills_per_repo": args.skills, "skills": args.repos * args.skills,
                  "changed": changed, "machine": "%s %s, %d CPUs" % (platform.system(), platform.machine(), os.cpu_count() or 0),
                  "git": subprocess.run([REAL_GIT, "--version"], capture_output=True, text=True).stdout.strip(),
                  "phases": rows}
        if args.json:
            with open(args.json, "w") as f:
                json.dump(result, f, indent=2)
                f.write("\n")
    finally:
        if args.keep:
            print("kept", root)
        else:
            shutil.rmtree(root, ignore_errors=True)


main()
