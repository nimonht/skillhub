#!/usr/bin/env python3
"""Offline test of every kind of source: GitLab with nested groups, a plain git server, a branch or tag, a folder inside a
repository, a local folder as a copy and as a live link, and import files that try to reach this machine.

usage: python3 tests/sources_smoke.py PATH/TO/skillhub[-cli.exe]

Remote hosts are local bare repositories behind url.insteadOf in a throw-away git config, the way the benches work.
"""
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

BIN = os.path.abspath(sys.argv[1])
WIN = os.name == "nt"
ROOT = os.path.realpath(tempfile.mkdtemp(prefix="skillhub-sources-"))
GITCONFIG = os.path.join(ROOT, "gitconfig")
GITLAB = "https://gitlab.com/group/sub/proj"
PLAIN = "https://git.example.test/team/tools.git"


def fail(msg):
    print("FAIL:", msg, file=sys.stderr)
    sys.exit(1)


def check(cond, msg):
    if not cond:
        fail(msg)


def git(cwd, *args):
    env = dict(os.environ, GIT_AUTHOR_DATE="2026-01-01T00:00:00Z", GIT_COMMITTER_DATE="2026-01-01T00:00:00Z",
               GIT_CONFIG_GLOBAL=os.path.join(ROOT, "no-gitconfig"), GIT_CONFIG_NOSYSTEM="1")
    p = subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.com", "-c", "core.autocrlf=false", "-c", "init.defaultBranch=main", *args],
                       cwd=cwd, capture_output=True, text=True, timeout=60, env=env)
    if p.returncode != 0:
        fail("git %s: %s" % (" ".join(args), p.stderr))
    return p.stdout.strip()


def fwd(path):
    """A path as Skill Hub stores it: with / on every platform."""
    return path.replace("\\", "/")


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n", encoding="utf-8") as f:
        f.write(text)


def skill(path, name, body="body"):
    write(os.path.join(path, "SKILL.md"), "---\nname: %s\ndescription: test skill %s\n---\n\n%s\n" % (name, name, body))


def commit(work, message="change"):
    git(work, "add", "-A")
    git(work, "commit", "-q", "-m", message)
    git(work, "push", "-q", "origin", "HEAD")


def make_upstream(rel):
    """A bare repository below ROOT/hosts, and a working copy to change it from."""
    bare = os.path.join(ROOT, "hosts", rel)
    work = os.path.join(ROOT, "work", rel.replace("/", "_"))
    os.makedirs(bare)
    git(bare, "init", "-q", "--bare", ".")
    os.makedirs(work)
    git(work, "init", "-q", ".")
    git(work, "remote", "add", "origin", bare)
    return work


def make_home(name):
    home = os.path.join(ROOT, name)
    os.makedirs(home + "/provbin")
    stub, body = ("codex.cmd", "@exit /b 0\r\n") if WIN else ("codex", "#!/bin/sh\nexit 0\n")
    write(home + "/provbin/" + stub, body)
    os.chmod(home + "/provbin/" + stub, 0o755)
    os.makedirs(home + "/.claude")  # Claude Code: an agent with its own folder, so links show up
    return home, dict(os.environ, HOME=home, USERPROFILE=home, SKILLHUB_HOME=home, XDG_CONFIG_HOME=home + "/.config", XDG_CACHE_HOME=home + "/.cache",
                      XDG_STATE_HOME=home + "/.local/state", XDG_DATA_HOME=home + "/.local/share", SKILLHUB_BIN_PATH=home + "/provbin",
                      SKILLHUB_CACHE=home + "/cache/skillhub", SKILLHUB_CONFIG=home + "/config/skillhub", GIT_CONFIG_GLOBAL=GITCONFIG,
                      GIT_CONFIG_NOSYSTEM="1")


def run(env, *args, expect=0):
    p = subprocess.run([BIN, *args], env=env, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=180, stdin=subprocess.DEVNULL)
    if expect is not None and p.returncode != expect:
        fail("`skillhub %s` exited %d (wanted %d)\n%s\n%s" % (" ".join(args), p.returncode, expect, p.stdout, p.stderr))
    return p


def lock(home):
    with open(home + "/.agents/.skill-lock.json", encoding="utf-8") as f:
        return json.load(f)["skills"]


def trees(home):
    with open(home + "/cache/skillhub/sources.json", encoding="utf-8") as f:
        return json.load(f)["trees"]["user"]


def listed(env):
    return {s["name"]: s for s in json.loads(run(env, "list", "--json").stdout)}


def rpc(env, *requests):
    """One backend session: each request in turn, each job until it is done. The replies and every jobDone, by request."""
    p = subprocess.Popen([BIN, "serve", "--stdio"], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=None if os.environ.get("SKILLHUB_TEST_STDERR") else subprocess.DEVNULL)

    def ask(i, method, params):
        p.stdin.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": method, "params": params}) + "\n").encode())
        p.stdin.flush()
        reply, job, done = None, None, None
        while True:
            line = p.stdout.readline()
            check(line, "the backend closed its output during " + method)
            m = json.loads(line)
            if m.get("id") == i:
                reply = m
                job = (m.get("result") or {}).get("job")
                if not job:
                    return reply, None
            elif m.get("method") == "event.jobDone" and m["params"]["job"] == job:
                done = m["params"]
            if reply and done:
                return reply, done

    try:
        ask(0, "initialize", {"protocol": 1, "client": "sources_smoke"})
        return [ask(i + 1, m, ps) for i, (m, ps) in enumerate(requests)]
    finally:
        p.stdin.close()
        p.wait(timeout=30)


def hostile_import(home, env):
    skills = []

    def bad(name, **fields):
        skills.append(dict({"name": name, "description": "x", "local": False, "path": "SKILL.md", "hash": ""}, **fields))

    secret = os.path.join(ROOT, "secret")
    skill(secret, "secret")
    bad("h-local", sourceType="local", source=secret)
    bad("h-local-as-git", sourceType="git", source=secret)
    bad("h-file", sourceType="git", source=pathlib.Path(secret).as_uri())
    bad("h-ssh", sourceType="git", source="ssh://git@example.test/x/y.git")
    bad("h-scp", sourceType="git", source="git@example.test:x/y.git")
    bad("h-ext", sourceType="git", source="ext::sh -c touch${IFS}pwned")
    bad("h-token", sourceType="git", source="https://user:token@example.test/x/y.git")
    bad("h-http", sourceType="gitlab", source="http://example.test/x/y.git")
    bad("h-https-as-github", source="https://example.test/x/y.git")          # a version 1 file holds GitHub sources only
    bad("h-unknown", sourceType="well-known", source="https://example.test/x/y.git")
    bad("h-option", sourceType="git", source=PLAIN, ref="--upload-pack=touch pwned")
    bad("h-subpath", sourceType="gitlab", source=GITLAB + "/-/tree/main/skills")
    bad("h-escape", sourceType="git", source=PLAIN, path="../SKILL.md")
    path = home + "/hostile.json"
    write(path, json.dumps({"format": "skillhub-export", "version": 2, "skills": skills}))
    before = set(listed(env))
    p = run(env, "import", path, "--yes")  # as in 1.x: every skill refused is "nothing to install", not an error
    check("nothing to install" in p.stdout, "a hostile import:\n" + p.stdout)
    check(set(listed(env)) == before, "a hostile import installed something")
    text = p.stdout + p.stderr
    for s in skills:
        check(s["name"] in text, "%s was not reported as refused:\n%s" % (s["name"], text))
    check(not list(pathlib.Path(ROOT).rglob("pwned")), "a hostile import ran a command")


def serve_hosts():
    """The throw-away git config that puts the fake hosts at bare repositories below ROOT/hosts."""
    hosts = pathlib.Path(ROOT, "hosts").as_uri()
    os.makedirs(os.path.join(ROOT, "hosts"))
    write(GITCONFIG, '[url "%s/gitlab/"]\n\tinsteadOf = https://gitlab.com/\n[url "%s/github/"]\n\tinsteadOf = https://github.com/\n'
                     '[url "%s/plain/"]\n\tinsteadOf = https://git.example.test/\n[protocol "file"]\n\tallow = always\n' % (hosts, hosts, hosts))


def main():
    serve_hosts()

    # ---- GitLab: nested groups, discovery, check, update ----
    lab = make_upstream("gitlab/group/sub/proj.git")
    for n in ("alpha", "beta"):
        skill(lab + "/skills/" + n, n)
    write(lab + "/README.md", "readme\n")
    commit(lab)
    home, env = make_home("home")
    p = run(env, "add", GITLAB)
    check("alpha" in p.stdout and "beta" in p.stdout, "listing a GitLab project:\n" + p.stdout)
    run(env, "add", GITLAB, "--all")
    entry = lock(home)["alpha"]
    check(entry["sourceType"] == "gitlab" and entry["source"] == GITLAB + ".git", "lock entry: %s" % entry)
    check(re.fullmatch("[0-9a-f]{64}", entry["skillFolderHash"]), "GitLab lock entries hold the folder hash: %s" % entry)
    tree = git(lab, "rev-parse", "HEAD:skills/alpha")
    check(trees(home)["alpha"]["treeSha"] == tree, "the tree record: %s" % trees(home))
    check(listed(env)["alpha"]["source"] == GITLAB + ".git", "list shows the source")
    check(run(env, "check").returncode == 0, "nothing changed yet")
    write(lab + "/README.md", "other\n")
    commit(lab)
    check(run(env, "check").returncode == 0, "a change outside the skill is no update")
    skill(lab + "/skills/alpha", "alpha", "new body")
    commit(lab)
    p = run(env, "check", expect=10)
    check("alpha" in p.stdout and "beta" not in p.stdout, "only alpha changed:\n" + p.stdout)
    run(env, "update", "--all")
    check("new body" in open(home + "/.agents/skills/alpha/SKILL.md", encoding="utf-8").read(), "the update did not arrive")
    check(lock(home)["alpha"]["skillFolderHash"] != entry["skillFolderHash"], "the hash in the lock did not change")
    check(trees(home)["alpha"]["treeSha"] == git(lab, "rev-parse", "HEAD:skills/alpha"), "the tree record did not follow")
    check(run(env, "check").returncode == 0, "updated, nothing more to do")

    # a copy npx installed: a folder hash and no tree record. Skill Hub cannot compare, and offers the update once.
    os.remove(home + "/cache/skillhub/sources.json")
    run(env, "check", expect=10)
    run(env, "update", "--all")
    check(run(env, "check").returncode == 0, "after one update the tree is on record")
    run(env, "remove", "alpha")
    run(env, "remove", "beta")

    # ---- a branch, a tag, a folder inside the repository ----
    git(lab, "checkout", "-q", "-b", "dev")
    skill(lab + "/skills/gamma", "gamma", "on dev")
    commit(lab)
    git(lab, "tag", "-a", "v1", "-m", "one")
    git(lab, "push", "-q", "origin", "v1")
    git(lab, "checkout", "-q", "main")
    run(env, "add", GITLAB + "#dev", "--skill", "gamma")
    entry = lock(home)["gamma"]
    check(entry["ref"] == "dev" and entry["source"] == GITLAB + ".git", "branch entry: %s" % entry)
    skill(lab + "/skills/alpha", "alpha", "only on main")
    commit(lab)
    check(run(env, "check").returncode == 0, "main moved, the branch did not")
    git(lab, "checkout", "-q", "dev")
    skill(lab + "/skills/gamma", "gamma", "dev again")
    commit(lab)
    git(lab, "checkout", "-q", "main")
    run(env, "check", expect=10)
    run(env, "update", "--all")
    check("dev again" in open(home + "/.agents/skills/gamma/SKILL.md", encoding="utf-8").read(), "the branch update did not arrive")
    run(env, "remove", "gamma")
    run(env, "add", GITLAB + "#v1", "--skill", "gamma")
    check("on dev" in open(home + "/.agents/skills/gamma/SKILL.md", encoding="utf-8").read(), "the tag was not followed")
    check(run(env, "check").returncode == 0, "an annotated tag that did not move")
    run(env, "remove", "gamma")
    p = run(env, "add", GITLAB + "/-/tree/main/skills/beta")
    check("beta" in p.stdout and "alpha" not in p.stdout, "a folder limits discovery:\n" + p.stdout)
    run(env, "add", GITLAB + "/-/tree/main/skills/beta", "--all")
    run(env, "add", GITLAB + "#nosuchbranch", "--all", expect=1)
    check("gamma" not in listed(env), "a missing branch installed something")
    run(env, "add", GITLAB + "#dev", "--skill", "gamma")  # stays for the export below

    # ---- a plain git server ----
    tools = make_upstream("plain/team/tools.git")
    skill(tools, "tool-skill")
    commit(tools)
    run(env, "add", PLAIN, "--all")
    entry = lock(home)["tool-skill"]
    check(entry["sourceType"] == "git" and entry["source"] == PLAIN, "git entry: %s" % entry)
    check(os.path.isfile(home + "/.agents/skills/tool-skill/SKILL.md"), "git skill missing")

    # ---- a local folder, as a copy ----
    folder = os.path.join(ROOT, "authoring")
    skill(folder + "/copied", "copied")
    skill(folder + "/linked", "linked")
    skill(folder + "/nested/deep/other", "other")
    os.makedirs(folder + "/node_modules/pkg")
    skill(folder + "/node_modules/pkg/hidden", "hidden")
    p = run(env, "add", folder)
    check(all(n in p.stdout for n in ("copied", "linked", "other")) and "hidden" not in p.stdout, "a folder's skills:\n" + p.stdout)
    run(env, "add", folder, "--skill", "copied")
    entry = lock(home)["copied"]
    check(entry["sourceType"] == "local" and entry["source"] == fwd(folder), "local entry: %s" % entry)
    check(re.fullmatch("[0-9a-f]{64}", entry["skillFolderHash"]), "local entries hold the folder hash: %s" % entry)
    check(not os.path.islink(home + "/.agents/skills/copied"), "a copy must be a real folder")
    check(run(env, "check").returncode == 0, "the folder did not change")
    skill(folder + "/copied", "copied", "edited")
    check("copied" in run(env, "check", expect=10).stdout, "a changed folder offers an update")
    check("edited" not in open(home + "/.agents/skills/copied/SKILL.md", encoding="utf-8").read(), "a copy must not follow the folder by itself")
    run(env, "update", "--all")
    check("edited" in open(home + "/.agents/skills/copied/SKILL.md", encoding="utf-8").read(), "the folder's update did not arrive")
    check(os.path.isfile(folder + "/copied/SKILL.md"), "an update must leave the folder alone")
    check(run(env, "check").returncode == 0, "the copy is current")

    # ---- a local folder, as a live link ----
    run(env, "add", folder, "--skill", "linked", "--link")
    central = home + "/.agents/skills/linked"
    check(lock(home)["linked"]["sourceType"] == "local", "the link is in the lock")
    check(listed(env)["linked"]["source"] == fwd(folder), "a live link is listed as a skill: %s" % listed(env).get("linked"))
    check(os.path.samefile(central, folder + "/linked"), "the entry must lead to the folder")
    skill(folder + "/linked", "linked", "live edit")
    check("live edit" in open(home + "/.claude/skills/linked/SKILL.md", encoding="utf-8").read(), "edits must reach an agent at once")
    check(run(env, "check").returncode == 0, "a live link is always current")
    check(run(env, "update", "--all").returncode == 0, "update with a live link")
    check(os.path.samefile(central, folder + "/linked"), "update must not replace a live link")
    check("MISSING" not in run(env, "doctor").stdout, "doctor on a live link")
    report = run(env, "adopt")
    check("linked" not in report.stdout or "0 change" in report.stdout, "a live link is no stray:\n" + report.stdout)
    check(os.path.samefile(central, folder + "/linked"), "adopt must not move a live link")

    # ---- an export carries remote sources, and local folders as files ----
    path = home + "/export.json"
    run(env, "export", path)
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    by = {s["name"]: s for s in data["skills"]}
    check(data["version"] == 2, "export version")
    check(by["beta"]["sourceType"] == "gitlab" and by["beta"]["source"] == GITLAB + ".git", "export entry: %s" % by["beta"])
    check(by["gamma"]["ref"] == "dev", "export entry: %s" % by["gamma"])
    check(by["copied"].get("local") and by["linked"].get("local"), "local folders must travel as files: %s" % sorted(by))
    check(fwd(folder) not in json.dumps(data), "an export must not name a local folder")
    other, env2 = make_home("home2")
    run(env2, "import", path, "--yes")
    now = listed(env2)
    check({"beta", "tool-skill", "copied", "linked"} <= set(now), "import: %s" % sorted(now))
    check(lock(other)["beta"]["sourceType"] == "gitlab", "the imported skill keeps its source: %s" % lock(other)["beta"])
    check(lock(other)["gamma"]["ref"] == "dev", "the imported skill keeps its branch: %s" % lock(other)["gamma"])
    check("dev again" in open(other + "/.agents/skills/gamma/SKILL.md", encoding="utf-8").read(), "the import did not follow the branch")
    check(not os.path.islink(other + "/.agents/skills/linked"), "an imported live link is a plain local skill")
    check("live edit" in open(other + "/.agents/skills/linked/SKILL.md", encoding="utf-8").read(), "embedded files missing")

    # ---- removing a live link leaves the folder ----
    run(env, "remove", "linked")
    check(not os.path.lexists(central), "the link is still there")
    check(os.path.isfile(folder + "/linked/SKILL.md"), "remove must leave the linked folder alone")

    # ---- the folder went away ----
    run(env, "add", folder, "--skill", "linked", "--link")
    moved = folder + "-moved"
    os.rename(folder, moved)
    p = run(env, "check", expect=None)
    check("cannot read" in p.stdout + p.stderr or "not found" in p.stdout + p.stderr, "a folder that went away:\n" + p.stdout + p.stderr)
    os.rename(moved, folder)
    run(env, "remove", "linked")

    # ---- inputs git must never see ----
    run(env, "add", "ext::sh -c touch${IFS}pwned", "--all", expect=None)
    p = run(env, "add", "-oops", "--all", expect=None)
    check(p.returncode != 0, "an option as a source")
    check(not list(pathlib.Path(ROOT).rglob("pwned")), "ext:: ran a command")
    hostile_import(home, env)

    # ---- a repository that wants a login: git asks nobody, Skill Hub says what to set up ----
    if not WIN:
        write(ROOT + "/stub/git", "#!/bin/sh\necho \"fatal: could not read Username for 'https://git.example.test': terminal prompts disabled\" >&2\nexit 128\n")
        os.chmod(ROOT + "/stub/git", 0o755)
        p = run(dict(env, PATH=ROOT + "/stub:" + env["PATH"]), "add", "https://git.example.test/private/repo.git", expect=1)
        check("credential helper" in p.stdout + p.stderr, "no hint about logging in:\n" + p.stdout + p.stderr)

    # ---- over the backend ----
    (_, found) = rpc(env, ("sources.discover", {"input": folder}))[0]
    check(found["ok"] and found["sourceType"] == "local" and found["host"] == "", "discover: %s" % found)
    (_, found) = rpc(env, ("sources.discover", {"input": GITLAB}))[0]
    check(found["ok"] and found["sourceType"] == "gitlab" and found["host"] == "gitlab.com", "discover: %s" % found)
    (_, done) = rpc(env, ("skills.install", {"source": folder, "skills": ["linked"], "mode": "link"}))[0]
    check(done["ok"], "install over RPC: %s" % done)
    (snapshot, _) = rpc(env, ("state.snapshot", {"page": 0, "pageSize": 1000}))[0]  # a new backend: its first scan
    rows = {r["name"]: r for r in snapshot["result"]["skills"]}
    check(rows["linked"]["live"] and rows["linked"]["sourceType"] == "local", "the row: %s" % rows["linked"])
    check(rows["beta"]["sourceType"] == "gitlab" and not rows["beta"]["live"], "the row: %s" % rows["beta"])

    print("sources smoke test passed (%s)" % sys.platform)


if __name__ == "__main__":
    try:
        main()
    finally:
        shutil.rmtree(ROOT, ignore_errors=True)
