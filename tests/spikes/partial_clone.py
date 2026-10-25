#!/usr/bin/env python3
"""Spike 2 of the 2.0 plan: can the engine check and update from partial shallow clones without fetching blobs one at
a time? Prints, for each step, how many fetches git started and how long it took.

usage: python3 tests/spikes/partial_clone.py [REMOTE_URL]
       (no URL: a local repository over file://; with a URL, for example https://github.com/anthropics/skills.git)
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

REMOTE = sys.argv[1] if len(sys.argv) > 1 else ""
GIT = shutil.which("git")


def run(args, cwd=None, stdin=None, trace=None):
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0")
    if trace:
        env["GIT_TRACE2_EVENT"] = trace
    t = time.perf_counter()
    p = subprocess.run([GIT, *args], cwd=cwd, input=stdin, capture_output=True, env=env)
    if p.returncode != 0:
        sys.exit("git %s failed: %s" % (" ".join(args), p.stderr.decode(errors="replace")))
    return p.stdout, time.perf_counter() - t


def fetches(trace):
    """Child `git fetch` processes started (lazy fetches show up as children of the command)."""
    n = 0
    if os.path.exists(trace):
        for line in open(trace, encoding="utf-8", errors="replace"):
            try:
                e = json.loads(line)
            except ValueError:
                continue
            if e.get("event") == "child_start" and "fetch" in e.get("argv", []):
                n += 1
        os.remove(trace)
    return n


def local_remote(root):
    up = os.path.join(root, "upstream.git")
    for s in range(40):
        d = os.path.join(up, "skills", "s%02d" % s)
        os.makedirs(os.path.join(d, "ref"))
        with open(os.path.join(d, "SKILL.md"), "w") as f:
            f.write("---\nname: s%02d\ndescription: skill %d\n---\nbody\n" % (s, s))
        for k in range(5):
            with open(os.path.join(d, "ref", "f%d.md" % k), "w") as f:
                f.write("file %d of skill %d\n" % (k, s) * 50)
    run(["init", "-q", "-b", "main"], cwd=up)
    run(["config", "uploadpack.allowFilter", "true"], cwd=up)
    run(["config", "uploadpack.allowAnySHA1InWant", "true"], cwd=up)
    run(["-c", "user.name=s", "-c", "user.email=s@x", "add", "-A"], cwd=up)
    run(["-c", "user.name=s", "-c", "user.email=s@x", "commit", "-q", "-m", "one"], cwd=up)
    return "file://" + up


def main():
    root = os.path.realpath(tempfile.mkdtemp(prefix="skillhub-spike-"))
    trace = os.path.join(root, "trace.json")
    try:
        url = REMOTE or local_remote(root)
        print("remote:", url, "|", run(["--version"])[0].decode().strip())
        clone = os.path.join(root, "clone")

        _, t = run(["ls-remote", url, "HEAD"])
        print("ls-remote HEAD                         %6.2f s" % t)

        _, t = run(["clone", "-q", "--depth", "1", "--filter=blob:none", "--no-checkout", "--", url, clone], trace=trace)
        print("clone --depth 1 --filter=blob:none     %6.2f s   fetches %d" % (t, fetches(trace)))

        out, t = run(["ls-tree", "-r", "-t", "-z", "HEAD"], cwd=clone, trace=trace)
        entries = [e for e in out.split(b"\0") if e]
        blobs = [e.split(b"\t", 1) for e in entries if e.split(b" ")[1] == b"blob"]
        trees = [e for e in entries if e.split(b" ")[1] == b"tree"]
        print("ls-tree -r -t (no sizes)               %6.2f s   fetches %d   %d trees, %d blobs listed" % (t, fetches(trace), len(trees), len(blobs)))

        skill_md = [head.split(b" ")[2].decode() for head, path in blobs if path.endswith(b"SKILL.md")]
        others = [head.split(b" ")[2].decode() for head, path in blobs if not path.endswith(b"SKILL.md")]

        # reading a few missing blobs the naive way: does cat-file fetch them one by one?
        sample = others[:5]
        _, t = run(["cat-file", "--batch"], cwd=clone, stdin=("\n".join(sample) + "\n").encode(), trace=trace)
        print("cat-file --batch, 5 missing blobs      %6.2f s   fetches %d" % (t, fetches(trace)))

        # the plan: prefetch every blob of the folders we need in one fetch, then read them locally
        want = skill_md + others[5:]
        _, t = run(["-c", "fetch.negotiationAlgorithm=noop", "fetch", "-q", "--no-tags", "--no-write-fetch-head",
                    "--recurse-submodules=no", "--filter=blob:none", "--stdin", "origin"], cwd=clone,
                   stdin=("\n".join(want) + "\n").encode(), trace=trace)
        print("fetch --stdin, %4d blobs in one go     %6.2f s   fetches %d" % (len(want), t, fetches(trace)))

        _, t = run(["cat-file", "--batch"], cwd=clone, stdin=("\n".join(want) + "\n").encode(), trace=trace)
        print("cat-file --batch, all prefetched       %6.2f s   fetches %d" % (t, fetches(trace)))

        _, t = run(["ls-tree", "-r", "-l", "-z", "HEAD"], cwd=clone, trace=trace)
        print("ls-tree -r -l (sizes), all present     %6.2f s   fetches %d" % (t, fetches(trace)))
    finally:
        shutil.rmtree(root, ignore_errors=True)


main()
