#!/usr/bin/env python3
"""How long the window's interface thread can stall while the backend works on 10,000 skills (target: 20 ms or less).

usage: python3 bench/bench_gui.py BUILD_DIR [--repos N] [--skills M] [--changed PERCENT]

Builds the fixture of bench/bench.py, installs every skill with one `skillhub add`, changes PERCENT % of them upstream,
then starts the window headless with `--bench`: once its first snapshot is in, it runs a check (whose delta changes
every row) and then updates everything available, and prints the longest gap between two runs of its event loop during
each.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

ap = argparse.ArgumentParser()
ap.add_argument("build")
ap.add_argument("--repos", type=int, default=500)
ap.add_argument("--skills", type=int, default=20)
ap.add_argument("--changed", type=float, default=10.0)
args = ap.parse_args()
BUILD = os.path.abspath(args.build)
CLI = os.path.join(BUILD, "skillhub.exe" if os.name == "nt" else "skillhub")
if sys.platform == "darwin":
    CLI = os.path.join(BUILD, "skillhub-gui.app", "Contents", "MacOS", "skillhub")
GUI = os.path.join(BUILD, "skillhub-gui.app/Contents/MacOS/skillhub-gui") if sys.platform == "darwin" else os.path.join(BUILD, "skillhub-gui")
REAL_GIT = shutil.which("git")


def git(cwd, *a):
    subprocess.run([REAL_GIT, "-c", "user.name=b", "-c", "user.email=b@example.com", "-c", "init.defaultBranch=main", *a],
                   cwd=cwd, check=True, capture_output=True)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        f.write(text)


def main():
    root = os.path.realpath(tempfile.mkdtemp(prefix="skillhub-bench-gui-"))
    try:
        t0 = time.perf_counter()
        repos = []
        for r in range(args.repos):
            path = os.path.join(root, "fixture", "repo%04d.git" % r)
            for s in range(args.skills):
                write(os.path.join(path, "skills", "r%04ds%03d" % (r, s), "SKILL.md"),
                      "---\nname: r%04ds%03d\ndescription: benchmark skill %d of repository %d\n---\n\nBody.\n" % (r, s, s, r))
            git(path, "init", "-q")
            git(path, "add", "-A")
            git(path, "commit", "-q", "-m", "first")
            repos.append(path)
        home = os.path.join(root, "home")
        os.makedirs(os.path.join(home, ".claude"))
        gitconfig = os.path.join(root, "gitconfig")
        write(gitconfig, '[url "file://%s/fixture/"]\n\tinsteadOf = https://github.com/bench/\n[protocol "file"]\n\tallow = always\n' % root)
        env = dict(os.environ, HOME=home, USERPROFILE=home, SKILLHUB_HOME=home, XDG_CONFIG_HOME=home + "/.config",
                   XDG_CACHE_HOME=home + "/.cache", XDG_STATE_HOME=home + "/.local/state", XDG_DATA_HOME=home + "/.local/share",
                   SKILLHUB_BIN_PATH=os.path.join(root, "none"), GIT_CONFIG_GLOBAL=gitconfig, GIT_CONFIG_NOSYSTEM="1",
                   QT_QPA_PLATFORM="offscreen", SKILLHUB_REDUCE_MOTION="1", SKILLHUB_NO_HOVER="1")
        subprocess.run([CLI, "add", *["bench/repo%04d" % r for r in range(args.repos)], "--all"], env=env, check=True, capture_output=True)
        want = max(1, int(args.repos * args.skills * args.changed / 100))
        touched = {}
        for i in range(want):
            touched.setdefault(i % args.repos, []).append((i // args.repos) % args.skills)
        for r, skills in touched.items():
            for s in skills:
                with open(os.path.join(repos[r], "skills", "r%04ds%03d" % (r, s), "SKILL.md"), "a", newline="\n") as f:
                    f.write("\nChanged upstream.\n")
            git(repos[r], "commit", "-q", "-am", "change")
        print("fixture: %d skills, %d changed upstream (%.1f s)" % (args.repos * args.skills, want, time.perf_counter() - t0))
        t = time.perf_counter()
        p = subprocess.run([GUI, "gui", "--bench"], env=env, capture_output=True, text=True, timeout=900)
        line = [l for l in p.stdout.splitlines() if l.startswith("max UI stall")]
        if p.returncode != 0 or not line:
            sys.exit("the window failed (%d):\n%s\n%s" % (p.returncode, p.stdout[-2000:], p.stderr[-2000:]))
        print("%s  (whole run %.1f s)" % (line[0], time.perf_counter() - t))

    finally:
        shutil.rmtree(root, ignore_errors=True)


main()
