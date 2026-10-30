#!/usr/bin/env python3
"""Offline test of project skills: <root>/.agents/skills, relative links for agents with a folder of their own,
skills-lock.json as npx writes it, updates, versions, what a lock file from a repository may not do, and the registry.

usage: python3 tests/projects_smoke.py PATH/TO/skillhub[-cli.exe]
"""
import hashlib
import json
import os
import shutil
import sys

from sources_smoke import ROOT, WIN, check, commit, fwd, listed, make_home, make_upstream, read, rpc, run, serve_hosts, skill, write

CLAUDE = "claude-code"


def folder_hash(path):
    """computedHash for names of plain ASCII letters, where JavaScript's order and byte order agree."""
    h = hashlib.sha256()
    files = []
    for d, _, names in os.walk(path):
        files += [os.path.relpath(os.path.join(d, n), path).replace("\\", "/") for n in names]
    for f in sorted(files):
        h.update(f.encode())
        with open(os.path.join(path, f), "rb") as data:
            h.update(data.read())
    return h.hexdigest()


def lock(project):
    return json.loads(read(project + "/skills-lock.json"))


def main():
    serve_hosts()
    hub = make_upstream("github/acme/skills.git")
    for n in ("zeta", "alpha"):
        skill(hub + "/skills/" + n, n)
    commit(hub)
    home, env = make_home("home")
    project = os.path.join(ROOT, "app")
    os.makedirs(project)
    p_args = ("--project", project)

    # ---- nothing is created until there is something to keep ----
    check("0 skills" in run(env, "list", *p_args).stdout, "an empty project")
    run(env, "adopt", *p_args)
    check(os.listdir(project) == [], "no empty folders in a repository: %s" % os.listdir(project))

    # ---- install ----
    run(env, "add", "acme/skills", "--all", *p_args)
    check(os.path.isfile(project + "/.agents/skills/alpha/SKILL.md"), "the project's shared folder")
    if not WIN:
        check(os.readlink(project + "/.claude/skills/alpha") == "../../.agents/skills/alpha", "a link inside the repository, relative")
    check(not os.path.exists(home + "/.agents/skills/alpha") and not os.path.exists(home + "/.claude/skills/alpha"), "the user's own skills stay as they are")
    raw = read(project + "/skills-lock.json")
    data = json.loads(raw)
    check(data["version"] == 1 and raw.endswith("}\n"), "as npx writes it:\n" + raw)
    check(list(data["skills"]) == ["alpha", "zeta"], "names sorted: %s" % list(data["skills"]))
    entry = data["skills"]["alpha"]
    check(entry["source"] == "acme/skills" and entry["sourceType"] == "github" and entry["skillPath"] == "skills/alpha/SKILL.md", "the entry: %s" % entry)
    check(entry["computedHash"] == folder_hash(project + "/.agents/skills/alpha"), "the hash of the files: %s" % entry)
    check("installedAt" not in entry and "skillFolderHash" not in entry, "no timestamps: they would conflict in every merge")
    rows = listed(dict(env))  # the user's own list stays empty
    check(rows == {}, "user list: %s" % rows)
    now = json.loads(run(env, "list", "--json", *p_args).stdout)
    by = {s["name"]: s for s in now}
    check(by["alpha"]["agents"][CLAUDE] == "link" and by["alpha"]["agents"]["codex"] == "shared", "the agents: %s" % by["alpha"])

    # ---- update, review, versions ----
    check(run(env, "check", *p_args).returncode == 0, "nothing changed")
    skill(hub + "/skills/alpha", "alpha", "new body")
    commit(hub)
    p = run(env, "check", *p_args, expect=10)
    check("alpha" in p.stdout and "zeta" not in p.stdout, "only alpha changed:\n" + p.stdout)
    check("+new body" in run(env, "diff", "alpha", *p_args).stdout, "diff in a project")
    run(env, "update", "--all", *p_args)
    new = lock(project)["skills"]["alpha"]
    check(new["computedHash"] == folder_hash(project + "/.agents/skills/alpha") and new["computedHash"] != entry["computedHash"], "the hash follows")
    check(run(env, "check", *p_args).returncode == 0, "current again")
    check("update" in run(env, "history", "alpha", *p_args).stdout, "history in a project")
    check("update" not in run(env, "history", "alpha").stdout, "the user's history is another one")
    run(env, "rollback", "alpha", *p_args)
    check(lock(project)["skills"]["alpha"]["computedHash"] == entry["computedHash"] and "new body" not in read(project + "/.agents/skills/alpha/SKILL.md"), "rollback")
    check("on hold" in run(env, "history", "alpha", *p_args).stdout, "a hold in a project")
    check("on hold" not in run(env, "history", "alpha").stdout, "holds are per scope")
    run(env, "update", "alpha", *p_args)
    check(not os.path.exists(project + "/.agents/.backup"), "backups stay out of the repository")
    run(env, "remove", "zeta", *p_args)
    check("zeta" not in lock(project)["skills"] and not os.path.lexists(project + "/.claude/skills/zeta"), "removed with its links")
    run(env, "restore", "zeta", *p_args)
    check("zeta" in lock(project)["skills"], "restored")

    # ---- a local folder inside the project ----
    skill(project + "/vendor/tool", "tool")
    run(env, "add", project + "/vendor", "--all", *p_args)
    entry = lock(project)["skills"]["tool"]
    check(entry["source"] == "./vendor/tool" or entry["source"] == "./vendor", "relative to the project: %s" % entry)
    check("sourceUrl" not in entry and "skillPath" not in entry, "npx keeps no URL for a local source: %s" % entry)
    check(run(env, "check", *p_args).returncode == 0, "the folder did not change")
    skill(project + "/vendor/tool", "tool", "edited")
    run(env, "check", *p_args, expect=10)
    run(env, "update", "--all", *p_args)
    check("edited" in read(project + "/.agents/skills/tool/SKILL.md"), "the local update")

    # ---- what npx wrote: a hash and no tree on record, so the update is offered once ----
    other = os.path.join(ROOT, "from-npx")
    skill(other + "/.agents/skills/zeta", "zeta")
    write(other + "/skills-lock.json", json.dumps({"version": 1, "skills": {"zeta": {
        "source": "acme/skills", "sourceType": "github", "skillPath": "skills/zeta/SKILL.md", "computedHash": folder_hash(other + "/.agents/skills/zeta"), "wellKnownDigest": "keep-me"}}}))
    run(env, "check", "--project", other, expect=10)
    run(env, "update", "--all", "--project", other)
    check(lock(other)["skills"]["zeta"]["wellKnownDigest"] == "keep-me", "unknown fields stay")
    check(run(env, "check", "--project", other).returncode == 0, "on record after one update")

    # ---- a lock file comes with the repository, and is not to be trusted ----
    hostile = os.path.join(ROOT, "hostile")
    secret = os.path.join(ROOT, "secret")
    skill(secret + "/stolen", "stolen", "private notes")
    skill(hostile + "/.agents/skills/stolen", "stolen", "bait")
    write(hostile + "/skills-lock.json", json.dumps({"version": 1, "skills": {
        "stolen": {"source": "../secret/stolen", "sourceType": "local", "computedHash": "0"},
        "viassh": {"source": "ext::sh -c touch${IFS}pwned", "sourceType": "git", "computedHash": "0"}}}))
    run(env, "update", "--all", "--project", hostile, expect=None)
    check("private notes" not in read(hostile + "/.agents/skills/stolen/SKILL.md"), "a project lock read a folder outside the project")
    check(not any("pwned" in f for _, _, fs in os.walk(ROOT) for f in fs), "a project lock ran a command")
    # the declared type proves nothing: where a source leads is what counts
    private = make_upstream("github/me/private.git")
    skill(private + "/leak", "leak", "private notes")
    commit(private)
    bare = os.path.join(ROOT, "hosts", "github", "me", "private.git")
    for name, entry in (("via-github", {"source": "acme/skills", "sourceType": "github", "sourceUrl": bare, "skillPath": "leak/SKILL.md"}),
                        ("via-dash", {"source": "acme/skills", "sourceType": "github", "sourceUrl": "--upload-pack=touch pwned", "skillPath": "leak/SKILL.md"}),
                        ("via-git", {"source": bare, "sourceType": "git", "skillPath": "leak/SKILL.md"}),
                        ("via-gitlab", {"source": bare, "sourceType": "gitlab", "sourceUrl": bare, "skillPath": "leak/SKILL.md"})):
        repo = os.path.join(ROOT, "lock-" + name)
        skill(repo + "/.agents/skills/leak", "leak", "bait")
        write(repo + "/skills-lock.json", json.dumps({"version": 1, "skills": {"leak": dict(entry, computedHash="0")}}))
        run(env, "check", "--project", repo, expect=None)
        run(env, "update", "--all", "--project", repo, expect=None)
        check("private notes" not in read(repo + "/.agents/skills/leak/SKILL.md"), "%s reached a repository on this machine" % name)
    check(not any("pwned" in f for _, _, fs in os.walk(ROOT) for f in fs), "a lock made git run a command")
    if not WIN:
        # a committed link: the text of the path stays inside, and the files are outside
        linked = os.path.join(ROOT, "lock-link")
        skill(linked + "/.agents/skills/stolen", "stolen", "bait")
        os.symlink(secret, linked + "/vendor")
        write(linked + "/skills-lock.json", json.dumps({"version": 1, "skills": {"stolen": {"source": "./vendor/stolen", "sourceType": "local", "computedHash": "0"}}}))
        run(env, "update", "--all", "--project", linked, expect=None)
        check("private notes" not in read(linked + "/.agents/skills/stolen/SKILL.md"), "a link in the repository led to a folder outside")
        # agent folders that lead out of the repository must not be written through
        real = os.path.join(ROOT, "real-claude")
        os.makedirs(real)
        out = os.path.join(ROOT, "lock-agent")
        os.makedirs(out)
        os.symlink(real, out + "/.claude")
        run(env, "add", "acme/skills", "--skill", "zeta", "--project", out)
        check(os.listdir(real) == [], "relink wrote through a link out of the repository: %s" % os.listdir(real))
        for victim, what in ((".agents", "shared folder"), ("skills-lock.json", "lock file")):
            trap = os.path.join(ROOT, "lock-trap-" + what.split()[0])
            os.makedirs(trap)
            target = os.path.join(ROOT, "trap-target-" + what.split()[0])
            if victim == ".agents":
                os.makedirs(target + "/skills")
            else:
                write(target, '{"version": 1, "skills": {}}\n')
            os.symlink(target, trap + "/" + victim)
            p = run(env, "add", "acme/skills", "--all", "--project", trap, expect=1)
            check("outside" in p.stderr, "a %s that leads out of the repository:\n%s" % (what, p.stderr))
            check(not os.path.exists(target + "/skills/zeta") and (victim == ".agents" or "zeta" not in read(target)), "written through the link")

    if not WIN:
        os.symlink(secret + "/stolen", hostile + "/.agents/skills/linked")
        write(hostile + "/skills-lock.json", json.dumps({"version": 1, "skills": {"linked": {"source": "./x", "sourceType": "local", "computedHash": "0"}}}))
        check("linked" not in run(env, "list", "--project", hostile).stdout, "a link in a repository is no skill")

    # ---- refused ----
    check("live link" in run(env, "add", project + "/vendor", "--all", "--link", *p_args, expect=2).stderr, "--link in a project")
    run(env, "disable", "alpha", *p_args, expect=2)
    run(env, "export", home + "/x.json", *p_args, expect=2)
    run(env, "list", "--project", os.path.join(ROOT, "nosuch"), expect=1)

    # ---- no symlinks here: copies kept identical ----
    nolinks = os.path.join(ROOT, "nolinks")
    os.makedirs(nolinks)
    env2 = dict(env, SKILLHUB_NO_SYMLINKS="1")
    run(env2, "add", "acme/skills", "--skill", "zeta", "--project", nolinks)
    copy = nolinks + "/.claude/skills/zeta"
    check(os.path.isfile(copy + "/SKILL.md") and not os.path.islink(copy), "a copy, not a link")
    state = {s["name"]: s for s in json.loads(run(env2, "list", "--json", "--project", nolinks).stdout)}
    check(state["zeta"]["agents"][CLAUDE] == "synced", "synced: %s" % state)
    check("healthy" in run(env2, "doctor", "--project", nolinks).stdout, "a copy is healthy")
    run(env2, "adopt", "--project", nolinks)
    check(os.path.isdir(copy) and os.path.isdir(nolinks + "/.agents/skills/zeta"), "a synced copy is no stray")
    write(copy + "/drift.md", "x\n")
    run(env2, "adopt", "--project", nolinks)
    check(not os.path.exists(copy + "/drift.md"), "relink keeps it identical")
    run(env2, "remove", "zeta", "--project", nolinks)
    check(not os.path.exists(copy), "the copy goes with the skill")

    # ---- a skill of yours, into a project ----
    run(env, "add", "acme/skills", "--skill", "alpha")
    team = os.path.join(ROOT, "team-app")
    os.makedirs(team)
    run(env, "copy", "alpha", team)
    check(os.path.isfile(team + "/.agents/skills/alpha/SKILL.md") and lock(team)["skills"]["alpha"]["source"] == "acme/skills", "copied with its source")
    check(run(env, "check", "--project", team).returncode == 0, "a copy is current")
    run(env, "copy", "alpha", team, expect=1)
    run(env, "copy", "nosuch", team, expect=1)

    # ---- over the backend ----
    replies = rpc(env, ("projects.add", {"path": team}), ("projects.list", {}), ("scope.set", {"scope": team}))
    check(replies[1][0]["result"]["projects"][0]["skills"] == 1, "projects.list: %s" % (replies[1],))
    check(team.replace("\\", "/") in replies[2][0]["result"]["central"], "scope.set: %s" % (replies[2],))
    replies = rpc(env, ("scope.set", {"scope": team}), ("skills.remove", {"name": "alpha"}))
    check(replies[1][1]["ok"] and not os.path.exists(team + "/.agents/skills/alpha") and os.path.exists(home + "/.agents/skills/alpha"), "a job runs in the scope: %s" % (replies[1],))
    (reply, _) = rpc(env, ("scope.set", {"scope": os.path.join(ROOT, "nosuch")}))[0]
    check("error" in reply, "scope.set on no folder: %s" % reply)
    replies = rpc(env, ("scope.set", {"scope": team}), ("scope.set", {"scope": "user"}))
    check(replies[1][0]["result"]["scope"] == "user", "back: %s" % (replies[1],))
    rpc(env, ("projects.remove", {"path": team}))

    # ---- the registry ----
    run(env, "projects", "add", project)
    run(env, "projects", "add", project)
    run(env, "projects", "add", os.path.join(ROOT, "nosuch"), expect=1)
    p = json.loads(run(env, "projects", "list", "--json").stdout)
    check(len(p) == 1 and p[0]["name"] == "app" and fwd(p[0]["path"]) == fwd(project) and not p[0]["autoUpdate"], "the list: %s" % p)
    skill(hub + "/skills/zeta", "zeta", "later")
    commit(hub)
    run(env, "update", "--all", "--auto")
    check("later" not in read(project + "/.agents/skills/zeta/SKILL.md"), "a project is not updated unless it asked")
    run(env, "projects", "auto", project, "on")
    p = run(env, "update", "--all", "--auto")
    check("later" in read(project + "/.agents/skills/zeta/SKILL.md"), "the daily update covers a project that asked:\n" + p.stdout)
    roots = os.path.join(ROOT, "code")
    skill(roots + "/team/thing/.agents/skills/x", "x")
    write(env["SKILLHUB_CONFIG"] + "/settings.json", json.dumps({"version": 1, "projects": {"scanRoots": [roots]}}))
    check("team/thing" in fwd(run(env, "projects", "list").stdout), "a project found below a root")
    run(env, "projects", "remove", project)
    run(env, "projects", "remove", project, expect=1)

    print("projects smoke test passed (%s)" % sys.platform)


if __name__ == "__main__":
    try:
        main()
    finally:
        shutil.rmtree(ROOT, ignore_errors=True)
