#!/usr/bin/env python3
"""Offline CLI smoke test for every platform (Linux, macOS, Windows): agent detection, adopt + links, remove,
export / import round trip, hostile import files, custom agents.

usage: python3 tests/portable_smoke.py PATH/TO/skillhub[-cli.exe]

It runs against a throw-away home (HOME and USERPROFILE). On Windows the app also keeps its settings in the real
AppData folder, which is why custom agents are removed again at the end.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

BIN = os.path.abspath(sys.argv[1])
WIN = os.name == "nt"
LINUX = sys.platform.startswith("linux")


def fail(msg):
    print("FAIL:", msg, file=sys.stderr)
    sys.exit(1)


def make_home():
    home = os.path.realpath(tempfile.mkdtemp(prefix="skillhub-test-"))  # long names: Windows temp dirs come as 8.3 short names
    env = dict(os.environ, HOME=home, USERPROFILE=home, SKILLHUB_HOME=home, XDG_CONFIG_HOME=home + "/.config", XDG_CACHE_HOME=home + "/.cache",
               XDG_STATE_HOME=home + "/.local/state", XDG_DATA_HOME=home + "/.local/share")
    stubs = os.path.join(home, "provbin")
    os.makedirs(stubs)
    name, body = ("codex.cmd", "@exit /b 0\r\n") if WIN else ("codex", "#!/bin/sh\nexit 0\n")
    with open(os.path.join(stubs, name), "w", newline="") as f:
        f.write(body)
    os.chmod(os.path.join(stubs, name), 0o755)
    env["SKILLHUB_BIN_PATH"] = stubs
    env["SKILLHUB_CACHE"] = home + "/cache/skillhub"
    env["SKILLHUB_CONFIG"] = home + "/config/skillhub"
    return home, env


def run(env, *args, expect=0):
    p = subprocess.run([BIN, *args], env=env, capture_output=True, text=True, timeout=180, stdin=subprocess.DEVNULL)
    if expect is not None and p.returncode != expect:
        fail("`skillhub %s` exited %d (wanted %d)\n%s\n%s" % (" ".join(args), p.returncode, expect, p.stdout, p.stderr))
    return p


def skill(path, name, body="body"):
    os.makedirs(path, exist_ok=True)
    with open(os.path.join(path, "SKILL.md"), "w", newline="\n") as f:
        f.write("---\nname: %s\ndescription: test skill %s\n---\n\n%s\n" % (name, name, body))


def states(env):
    return {s["name"]: s["agents"] for s in json.loads(run(env, "list", "--json").stdout)}


home, env = make_home()
home2 = None
try:
    central = os.path.join(home, ".agents", "skills")
    os.makedirs(os.path.join(home, ".claude"))          # Claude Code is "installed" through its folder
    skill(os.path.join(central, "alpha"), "alpha")
    skill(os.path.join(home, ".claude", "skills", "beta"), "beta", "stray")
    with open(os.path.join(central, "alpha", "run.sh"), "w", newline="\n") as f:
        f.write("#!/bin/sh\necho hi\n")
    os.chmod(os.path.join(central, "alpha", "run.sh"), 0o755)

    v = run(env, "--version").stdout
    if "skillhub" not in v:
        fail("--version printed " + v)

    provs = {p["id"]: p for p in json.loads(run(env, "providers", "--json").stdout)}
    if not provs.get("claude-code", {}).get("managed"):
        fail("Claude Code was not found through ~/.claude: %s" % list(provs))
    if not provs.get("codex", {}).get("managed"):
        fail("the stub `codex` command was not detected: %s" % list(provs))

    run(env, "adopt")
    if not os.path.isfile(os.path.join(central, "beta", "SKILL.md")):
        fail("the stray skill was not moved into the shared folder")
    st = states(env)
    if set(st) != {"alpha", "beta"}:
        fail("list shows %s" % list(st))
    for n in ("alpha", "beta"):
        if st[n].get("claude-code") != "link":
            fail("%s is not linked for Claude Code: %s" % (n, st[n]))
        if st[n].get("codex") != "shared":
            fail("%s is not shared with Codex: %s" % (n, st[n]))
        link = os.path.join(home, ".claude", "skills", n)
        if os.path.realpath(link) != os.path.realpath(os.path.join(central, n)):
            fail("%s does not lead to the shared folder" % link)
    if "healthy" not in run(env, "doctor").stdout:
        fail("doctor is not healthy after adopt")

    # remove: skill and link disappear, a backup stays
    run(env, "remove", "beta")
    if os.path.exists(os.path.join(central, "beta")) or os.path.lexists(os.path.join(home, ".claude", "skills", "beta")):
        fail("remove left files behind")
    if not os.path.isdir(os.path.join(home, ".agents", ".backup", "_removed")):
        fail("remove kept no backup")

    # a dead link that points into the shared folder is cleaned up; links to anything else stay
    if not WIN:
        os.symlink("../../.agents/skills/ghost", os.path.join(home, ".claude", "skills", "ghost"))
        os.symlink("/nonexistent/elsewhere", os.path.join(home, ".claude", "skills", "foreign"))
        run(env, "adopt")
        if os.path.lexists(os.path.join(home, ".claude", "skills", "ghost")):
            fail("a dead link into the shared folder was not removed")
        if not os.path.lexists(os.path.join(home, ".claude", "skills", "foreign")):
            fail("a foreign link was removed")
        os.remove(os.path.join(home, ".claude", "skills", "foreign"))

    # your own agent
    mine = os.path.join(home, "my-agent-skills")
    run(env, "providers", "add", "My Agent", mine)
    ids = [p["id"] for p in json.loads(run(env, "providers", "--json").stdout)]
    if "custom-my-agent" not in ids:
        fail("custom agent missing: %s" % ids)
    run(env, "providers", "remove", "custom-my-agent")

    # export / import
    skill(os.path.join(central, "gamma"), "gamma", "local skill with a nested file")
    os.makedirs(os.path.join(central, "gamma", "ref"))
    with open(os.path.join(central, "gamma", "ref", "data.bin"), "wb") as f:
        f.write(bytes(range(256)) * 4)
    export = os.path.join(home, "export.json")
    run(env, "export", export)
    data = json.load(open(export, encoding="utf-8"))
    if data.get("format") != "skillhub-export":
        fail("export has the wrong format")

    home2, env2 = make_home()
    os.makedirs(os.path.join(home2, ".claude"))
    run(env2, "import", export, "--yes")
    g = os.path.join(home2, ".agents", "skills", "gamma")
    if open(os.path.join(g, "ref", "data.bin"), "rb").read() != bytes(range(256)) * 4:
        fail("a binary file did not survive the round trip")
    if not WIN and not os.access(os.path.join(home2, ".agents", "skills", "alpha", "run.sh"), os.X_OK):
        fail("the executable bit was lost")
    if states(env2).get("gamma", {}).get("claude-code") != "link":
        fail("an imported skill is not linked for Claude Code")

    # a hostile file: path traversal, absolute path, bad name, non-GitHub source
    evil = json.loads(json.dumps(data))
    for s in evil["skills"]:
        if s.get("files"):
            s["files"][0]["path"] = "../escape.txt"
    evil["skills"].append({"name": "../../etc", "source": "owner/repo", "path": "x/SKILL.md"})
    evil["skills"].append({"name": "ok-name", "source": "https://evil.example/x.git", "path": "SKILL.md"})
    home3, env3 = make_home()
    try:
        bad = os.path.join(home3, "evil.json")
        json.dump(evil, open(bad, "w", encoding="utf-8"))
        run(env3, "import", bad, "--yes", expect=None)
        if os.path.exists(os.path.join(home3, ".agents", "escape.txt")) or os.path.exists(os.path.join(home3, "escape.txt")):
            fail("a hostile path escaped the skills folder")
        if os.path.isdir(os.path.join(home3, ".agents", "skills", "ok-name")):
            fail("a non-GitHub source was accepted")
    finally:
        shutil.rmtree(home3, ignore_errors=True)

    # install / check / update through git, offline: the "upstream" is a local repository the cache already cloned
    def git(cwd, *args):
        p = subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.com", "-c", "core.autocrlf=false", *args],
                           cwd=cwd, capture_output=True, text=True, timeout=60)
        if p.returncode != 0:
            fail("git %s: %s" % (" ".join(args), p.stderr))
        return p.stdout.strip()

    upstream = os.path.join(home, "upstream")
    skill(os.path.join(upstream, "skills", "one"), "one", "version 1")
    os.makedirs(os.path.join(upstream, "skills", "one", "sub"))
    with open(os.path.join(upstream, "skills", "one", "sub", "file.txt"), "w", newline="\n") as f:
        f.write("nested\n")
    with open(os.path.join(upstream, "skills", "one", "run.sh"), "w", newline="\n") as f:
        f.write("#!/bin/sh\necho one\n")
    skill(os.path.join(upstream, "skills", "two"), "two", "second skill")
    git(upstream, "init", "-q")
    git(upstream, "add", "-A")
    git(upstream, "update-index", "--chmod=+x", "skills/one/run.sh")
    git(upstream, "commit", "-q", "-m", "first")
    cached = os.path.join(env["SKILLHUB_CACHE"], "repos", "acme__skills")
    os.makedirs(os.path.dirname(cached))
    git(home, "clone", "-q", "--depth", "1", "file://" + upstream.replace("\\", "/") if WIN else upstream, cached)
    run(env, "add", "acme/skills", "--all")
    one = os.path.join(central, "one")
    if open(os.path.join(one, "sub", "file.txt"), newline="").read() != "nested\n":
        fail("a nested file was not installed")
    if not WIN and not os.access(os.path.join(one, "run.sh"), os.X_OK):
        fail("git's executable bit was not kept")
    lock = json.load(open(os.path.join(home, ".agents", ".skill-lock.json"), encoding="utf-8"))["skills"]
    if lock["one"]["source"] != "acme/skills" or lock["one"]["skillFolderHash"] != git(upstream, "rev-parse", "HEAD:skills/one"):
        fail("lock file entry is wrong: %s" % lock.get("one"))
    if states(env)["one"].get("claude-code") != "link":
        fail("an installed skill is not linked for Claude Code")
    run(env, "check", expect=0)                    # nothing new upstream
    with open(os.path.join(upstream, "skills", "one", "SKILL.md"), "w", newline="\n") as f:
        f.write("---\nname: one\ndescription: test skill one\n---\n\nversion 2\n")
    git(upstream, "commit", "-q", "-am", "second")
    run(env, "check", expect=10)                   # an update is available
    run(env, "update", "one")
    if "version 2" not in open(os.path.join(one, "SKILL.md"), encoding="utf-8").read():
        fail("update did not install the new version")
    if not os.path.isdir(os.path.join(home, ".agents", ".backup", "one")):
        fail("update kept no backup of the old version")
    run(env, "check", expect=0)

    # automation exists only where there are systemd user units
    if not LINUX:
        p = run(env, "autoupdate", "status", expect=1)
        if "not available" not in p.stderr:
            fail("autoupdate should say it is not available here: " + p.stderr)

    print("portable smoke test passed (%s)" % sys.platform)
finally:
    for h in (home, home2):
        if h:
            shutil.rmtree(h, ignore_errors=True)
