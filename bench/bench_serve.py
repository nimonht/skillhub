#!/usr/bin/env python3
"""Benchmark of `skillhub serve --stdio`, the window's backend, offline.

usage: python3 bench/bench_serve.py BINARY [--repos N] [--skills M] [--changed PERCENT] [--json FILE]

Builds the fixture of bench/bench.py (N local repositories of M skills, mapped to github.com/bench/<repo>), installs
every skill with one `skillhub add`, then starts the backend and measures what the window will feel:

  snapshot   the first full snapshot, page by page (1,000 skills a page): time and bytes
  check      skills.check with nothing changed upstream; the delta that follows changes every row (tracked -> current),
             the biggest one the window has to apply
  check-10   skills.check after PERCENT % of the skills changed upstream
  update     skills.update of everything available

and the backend's peak resident memory over the whole run (VmHWM, Linux only).
"""
import argparse
import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ap = argparse.ArgumentParser()
ap.add_argument("binary")
ap.add_argument("--repos", type=int, default=500)
ap.add_argument("--skills", type=int, default=20)
ap.add_argument("--changed", type=float, default=10.0)
ap.add_argument("--json", default="")
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
        path = os.path.join(root, "fixture", name + ".git")
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
    want = max(1, int(args.repos * args.skills * args.changed / 100))
    touched = {}
    for i in range(want):
        touched.setdefault(i % args.repos, []).append((i // args.repos) % args.skills)
    for r, skills in touched.items():
        name, path = repos[r]
        for s in skills:
            with open(os.path.join(path, "skills", "r%04ds%03d" % (r, s), "SKILL.md"), "a", newline="\n") as f:
                f.write("\nChanged upstream.\n")
        git(path, "commit", "-q", "-am", "change")
    return want


class Backend:
    def __init__(self, env):
        self.p = subprocess.Popen([BIN, "serve", "--stdio"], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL)
        self.q = queue.Queue()
        self.next_id = 0
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for raw in self.p.stdout:
            self.q.put((raw, json.loads(raw)))
        self.q.put(None)

    def request(self, method, params=None):
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            msg["params"] = params
        self.p.stdin.write((json.dumps(msg) + "\n").encode())
        self.p.stdin.flush()
        return self.next_id

    def until(self, match, timeout=600):
        """Messages up to and including the first one `match` accepts, as (raw bytes, parsed) pairs."""
        seen = []
        end = time.time() + timeout
        while time.time() < end:
            item = self.q.get(timeout=max(0.1, end - time.time()))
            if item is None:
                sys.exit("the backend ended")
            seen.append(item)
            if match(item[1]):
                return seen
        sys.exit("timed out")

    def call(self, method, params=None):
        rid = self.request(method, params)
        raw, msg = self.until(lambda m: m.get("id") == rid and "method" not in m)[-1]
        if "error" in msg:
            sys.exit("%s: %s" % (method, msg["error"]))
        return raw, msg["result"]

    def job(self, method, params=None):
        """Runs a job; returns its jobDone and every message that came with it."""
        _, result = self.call(method, params)
        seen = self.until(lambda m: m.get("method") == "event.jobDone" and m["params"]["job"] == result["job"])
        return seen[-1][1]["params"], seen

    def peak_mb(self):
        try:
            with open("/proc/%d/status" % self.p.pid) as f:
                for line in f:
                    if line.startswith("VmHWM:"):
                        return int(line.split()[1]) / 1024
        except OSError:
            pass
        return 0.0


def main():
    root = os.path.realpath(tempfile.mkdtemp(prefix="skillhub-bench-serve-"))
    try:
        t0 = time.perf_counter()
        repos = build_fixture(root)
        print("fixture: %d skills (%.1f s)" % (args.repos * args.skills, time.perf_counter() - t0))
        home = os.path.join(root, "home")
        os.makedirs(os.path.join(home, ".claude"))
        gitconfig = os.path.join(root, "gitconfig")
        with open(gitconfig, "w") as f:
            f.write('[url "file://%s/fixture/"]\n\tinsteadOf = https://github.com/bench/\n[protocol "file"]\n\tallow = always\n' % root)
        env = dict(os.environ, HOME=home, USERPROFILE=home, SKILLHUB_HOME=home, XDG_CONFIG_HOME=home + "/.config",
                   XDG_CACHE_HOME=home + "/.cache", XDG_STATE_HOME=home + "/.local/state", XDG_DATA_HOME=home + "/.local/share",
                   SKILLHUB_BIN_PATH=os.path.join(root, "none"), GIT_CONFIG_GLOBAL=gitconfig, GIT_CONFIG_NOSYSTEM="1")
        subprocess.run([BIN, "add", *["bench/" + n for n, _ in repos], "--all"], env=env, check=True, capture_output=True)

        b = Backend(env)
        b.call("initialize", {"protocol": 1})
        rows = []
        t = time.perf_counter()
        raw, first = b.call("state.snapshot", {"page": 0, "pageSize": 1000})
        size = len(raw)
        for page in range(1, first["pages"]):
            raw, p = b.call("state.snapshot", {"page": page, "pageSize": 1000})
            size += len(raw)
            rows += p["skills"]
        rows += first["skills"]
        snap = time.perf_counter() - t
        if len(rows) != args.repos * args.skills:
            sys.exit("snapshot has %d skills" % len(rows))
        result = {"skills": len(rows), "snapshot_seconds": round(snap, 2), "snapshot_mb": round(size / 1e6, 1)}
        print("snapshot   %6.2f s   %6.1f MB in %d pages" % (snap, size / 1e6, first["pages"]))

        def timed_job(label, method, params=None):
            t = time.perf_counter()
            done, seen = b.job(method, params)
            secs = time.perf_counter() - t
            delta = b.until(lambda m: m.get("method") == "state.delta")[-1][0]  # the rescan that follows the job
            print("%-10s %6.2f s   delta %6.1f MB   %s" % (label, secs, len(delta) / 1e6, json.dumps(done["summary"])))
            return secs, len(delta)

        secs, delta = timed_job("check", "skills.check")
        result.update(check_seconds=round(secs, 2), first_check_delta_mb=round(delta / 1e6, 1))
        result["changed"] = change_upstream(repos)
        secs, delta = timed_job("check-%d" % args.changed, "skills.check")
        result.update(check_changed_seconds=round(secs, 2))
        secs, delta = timed_job("update", "skills.update")
        result.update(update_seconds=round(secs, 2), update_delta_mb=round(delta / 1e6, 1))
        result["peak_rss_mb"] = round(b.peak_mb(), 1)
        print("backend peak RSS %.1f MB" % result["peak_rss_mb"])
        b.call("shutdown")
        b.p.stdin.close()
        b.p.wait(timeout=60)
        if args.json:
            with open(args.json, "w") as f:
                json.dump(result, f, indent=2)
                f.write("\n")
    finally:
        shutil.rmtree(root, ignore_errors=True)


main()
