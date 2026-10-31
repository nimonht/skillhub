#!/usr/bin/env python3
"""Many repositories at once, offline: the parallel engine under load. CI runs this under ThreadSanitizer.

usage: python3 tests/parallel_smoke.py PATH/TO/skillhub[-cli.exe] [REPOSITORIES] [SKILLS_PER_REPOSITORY]

Repositories are local folders behind url.insteadOf in a throw-away git config, and everything happens in a throw-away home.
"""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

BIN = os.path.abspath(sys.argv[1])
REPOS = int(sys.argv[2]) if len(sys.argv) > 2 else 30
SKILLS = int(sys.argv[3]) if len(sys.argv) > 3 else 5
ROOT = os.path.realpath(tempfile.mkdtemp(prefix="skillhub-parallel-"))
HOME = os.path.join(ROOT, "home")
GITCONFIG = os.path.join(ROOT, "gitconfig")


def fail(msg):
    print("FAIL:", msg, file=sys.stderr)
    sys.exit(1)


def check(cond, msg):
    if not cond:
        fail(msg)


def git(cwd, *args):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.com", "-c", "init.defaultBranch=main", *args],
                   cwd=cwd, check=True, capture_output=True)


def name_of(repo, skill):
    return "r%04ds%03d" % (repo, skill)


def build_fixture():
    for r in range(REPOS):
        path = os.path.join(ROOT, "fixture", "repo%04d.git" % r)   # a working repository whose folder name ends in .git
        for s in range(SKILLS):
            folder = pathlib.Path(path, "skills", name_of(r, s))
            folder.joinpath("reference").mkdir(parents=True)
            folder.joinpath("SKILL.md").write_text("---\nname: %s\ndescription: skill %d of repository %d\n---\n\nBody.\n" % (name_of(r, s), s, r))
            folder.joinpath("reference", "notes.md").write_text("notes %d %d\n" % (r, s) * 20)
        git(path, "init", "-q")
        git(path, "add", "-A")
        git(path, "commit", "-q", "-m", "first")


def change_upstream():
    """One skill in every other repository; returns their names."""
    changed = []
    for r in range(0, REPOS, 2):
        path = os.path.join(ROOT, "fixture", "repo%04d.git" % r)
        with open(os.path.join(path, "skills", name_of(r, 0), "SKILL.md"), "a", newline="\n") as f:
            f.write("\nChanged upstream.\n")
        git(path, "commit", "-q", "-am", "change")
        changed.append(name_of(r, 0))
    return changed


def start(env, *args):
    return subprocess.Popen([BIN, *args], env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def run(env, *args, expect=0):
    p = start(env, *args)
    out, err = p.communicate(timeout=300)
    check(p.returncode == expect, "`skillhub %s` exited %d, not %d\n%s\n%s" % (" ".join(args[:3]), p.returncode, expect, out[-2000:], err[-2000:]))
    return out


def main():
    build_fixture()
    os.makedirs(os.path.join(HOME, ".claude"))
    with open(GITCONFIG, "w") as f:
        f.write('[url "%s/"]\n\tinsteadOf = https://github.com/acme/\n[protocol "file"]\n\tallow = always\n' % pathlib.Path(ROOT, "fixture").as_uri())
    env = dict(os.environ, HOME=HOME, USERPROFILE=HOME, SKILLHUB_HOME=HOME, XDG_CONFIG_HOME=HOME + "/.config", XDG_CACHE_HOME=HOME + "/.cache",
               XDG_STATE_HOME=HOME + "/.local/state", XDG_DATA_HOME=HOME + "/.local/share", SKILLHUB_BIN_PATH=os.path.join(ROOT, "none"),
               GIT_CONFIG_GLOBAL=GITCONFIG, GIT_CONFIG_NOSYSTEM="1")
    central = os.path.join(HOME, ".agents", "skills")
    total = REPOS * SKILLS

    run(env, "add", *["acme/repo%04d" % r for r in range(REPOS)], "--all")
    listed = json.loads(run(env, "list", "--json"))
    check(len(listed) == total, "installed %d of %d skills" % (len(listed), total))
    check(all(s["source"].startswith("acme/repo") for s in listed), "a skill lost its source")
    check(all(os.path.islink(os.path.join(HOME, ".claude", "skills", s["name"])) for s in listed), "a skill has no link for Claude Code")
    run(env, "check")

    # several processes share the cache: per-repository locks keep them off each other's git files
    runs = [start(env, "check") for _ in range(3)]
    for p in runs:
        out, err = p.communicate(timeout=300)
        check(p.returncode == 0, "a check beside other checks exited %d\n%s\n%s" % (p.returncode, out[-1000:], err[-1000:]))

    changed = change_upstream()
    check(len(changed) > 0, "nothing changed upstream: too few repositories")
    run(env, "check", expect=10)
    # `check NAME` checks every tracked skill and its exit code counts them all, so an unchanged name still gets 10
    # (docs/formats.md): scripts may rely on it
    run(env, "check", name_of(1, 0), expect=10)

    run(env, "update", "--all")
    for r in range(REPOS):
        for s in range(SKILLS):
            text = pathlib.Path(central, name_of(r, s), "SKILL.md").read_text()
            check(("Changed upstream." in text) == (name_of(r, s) in changed), "%s holds the wrong version" % name_of(r, s))
    run(env, "check")
    check(len(json.loads(run(env, "list", "--json"))) == total, "an update lost a skill")
    run(env, "doctor")
    print("parallel test passed: %d repositories, %d skills" % (REPOS, total))


try:
    main()
finally:
    shutil.rmtree(ROOT, ignore_errors=True)
