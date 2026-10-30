#!/usr/bin/env python3
"""Offline test of the per-agent switches: one agent at a time, the agents that read the shared folder (parked skills),
what must never be touched, updates of parked skills, and the same over the backend.

usage: python3 tests/toggles_smoke.py PATH/TO/skillhub[-cli.exe]
"""
import json
import os
import shutil
import sys

from sources_smoke import ROOT, check, commit, listed, lock, make_home, make_upstream, read, rpc, run, serve_hosts, skill

CLAUDE = "claude-code"


def state(env):
    path = env["SKILLHUB_CONFIG"] + "/state.json"
    return json.loads(read(path)) if os.path.isfile(path) else {}


def link_of(home, name):
    return home + "/.claude/skills/" + name


def agents_of(env, name):
    return listed(env)[name]["agents"]


def main():
    serve_hosts()
    hub = make_upstream("github/acme/skills.git")
    for n in ("alpha", "beta", "gamma"):
        skill(hub + "/skills/" + n, n)
    commit(hub)
    home, env = make_home("home")
    run(env, "add", "acme/skills", "--all")
    check(os.path.islink(link_of(home, "alpha")), "linked at first")

    # ---- one agent ----
    run(env, "disable", "alpha", "--agent", CLAUDE)
    check(not os.path.lexists(link_of(home, "alpha")), "the link must go")
    check(agents_of(env, "alpha")[CLAUDE] == "off" and agents_of(env, "beta")[CLAUDE] == "link", "list: %s" % agents_of(env, "alpha"))
    check("alpha" in lock(home), "one agent does not touch the lock")
    check("healthy" in run(env, "doctor").stdout, "off is healthy")
    run(env, "adopt")
    check(not os.path.lexists(link_of(home, "alpha")), "repair must not bring it back")
    run(env, "enable", "alpha", "--agent", CLAUDE)
    check(os.path.islink(link_of(home, "alpha")) and agents_of(env, "alpha")[CLAUDE] == "link", "the link comes back")

    # ---- what is not ours stays ----
    run(env, "disable", "beta", "--agent", CLAUDE)
    os.makedirs(link_of(home, "beta"))
    write_md = link_of(home, "beta") + "/mine.md"
    with open(write_md, "w") as f:
        f.write("mine\n")
    run(env, "adopt")
    check(os.path.isfile(write_md), "a real folder is never touched")
    shutil.rmtree(link_of(home, "beta"))
    run(env, "enable", "beta", "--agent", CLAUDE)
    run(env, "adopt")
    run(env, "disable", "nosuch", expect=1)
    run(env, "disable", "alpha", "--agent", "nosuch", expect=1)
    check("shared folder" in run(env, "disable", "alpha", "--agent", "codex", expect=1).stderr, "an agent that reads the shared folder")

    # ---- the agents that read the shared folder: parked ----
    entry = lock(home)["alpha"]
    run(env, "disable", "alpha", "--shared")
    check(os.path.isdir(home + "/.agents/.parked/alpha") and not os.path.exists(home + "/.agents/skills/alpha"), "parked")
    check("alpha" not in lock(home), "npx must not see it in the lock")
    check(state(env)["parked"]["alpha"]["lockEntry"]["skillFolderHash"] == entry["skillFolderHash"], "the entry waits in state.json")
    now = agents_of(env, "alpha")
    check(now["codex"] == "off" and now[CLAUDE] == "link", "parked, and Claude Code keeps it: %s" % now)
    check("parked" in os.readlink(link_of(home, "alpha")).replace("\\", "/"), "the link leads to the parked folder")
    check(listed(env)["alpha"]["source"] == "acme/skills", "a parked skill keeps its source")

    # ---- a parked skill still updates, and stays parked ----
    skill(hub + "/skills/alpha", "alpha", "new body")
    commit(hub)
    run(env, "check", expect=10)
    p = run(env, "diff", "alpha", "--stat")
    check("update available" in p.stdout, "diff of a parked skill:\n" + p.stdout)
    run(env, "update", "alpha")
    check("new body" in read(home + "/.agents/.parked/alpha/SKILL.md"), "the update did not reach the parked folder")
    check(not os.path.exists(home + "/.agents/skills/alpha") and "alpha" not in lock(home), "an update must not unpark")
    check(state(env)["parked"]["alpha"]["lockEntry"]["skillFolderHash"] != entry["skillFolderHash"], "the waiting entry follows")
    run(env, "rollback", "alpha")
    check("new body" not in read(home + "/.agents/.parked/alpha/SKILL.md") and "alpha" not in lock(home), "rollback of a parked skill")
    run(env, "update", "alpha")

    # ---- back ----
    run(env, "enable", "alpha", "--shared")
    check(os.path.isdir(home + "/.agents/skills/alpha") and not os.path.exists(home + "/.agents/.parked/alpha"), "unparked")
    check(lock(home)["alpha"]["source"] == "acme/skills" and "alpha" not in state(env).get("parked", {}), "the entry is back")
    check(os.path.exists(link_of(home, "alpha") + "/SKILL.md"), "the link was pointed back")

    # ---- everywhere, by default ----
    run(env, "disable", "gamma")
    check(not os.path.lexists(link_of(home, "gamma")) and os.path.isdir(home + "/.agents/.parked/gamma"), "disabled everywhere")
    check(all(v == "off" for v in agents_of(env, "gamma").values()), "off everywhere: %s" % agents_of(env, "gamma"))
    check("gamma" in listed(env), "still listed")
    run(env, "export", home + "/e.json")
    data = json.loads(read(home + "/e.json"))
    check("gamma" in {s["name"] for s in data["skills"]}, "a parked skill is part of the export")
    run(env, "enable", "gamma")
    check(os.path.islink(link_of(home, "gamma")) and "gamma" in lock(home), "enabled everywhere")

    # ---- the shared folder wins ----
    run(env, "disable", "gamma", "--shared")
    skill(home + "/.agents/skills/gamma", "gamma", "installed by npx")
    run(env, "adopt")
    check("installed by npx" in read(home + "/.agents/skills/gamma/SKILL.md") and not os.path.exists(home + "/.agents/.parked/gamma"), "the shared folder wins")
    check(os.listdir(home + "/.agents/.backup/.parked-replaced"), "the parked copy is kept")
    check("gamma" not in state(env).get("parked", {}), "no entry left behind")

    # ---- removing and restoring ----
    run(env, "disable", "beta", "--shared")
    run(env, "remove", "beta")
    check("beta" not in listed(env) and "beta" not in state(env).get("parked", {}), "a parked skill can be removed")
    run(env, "restore", "beta")
    check(os.path.isdir(home + "/.agents/skills/beta") and "beta" in lock(home), "restored to the shared folder")
    run(env, "disable", "beta", "--agent", CLAUDE)
    run(env, "remove", "beta")
    run(env, "restore", "beta")
    check(os.path.islink(link_of(home, "beta")), "a switch does not outlive the skill")

    # ---- before going back to 1.x ----
    run(env, "disable", "alpha", "beta", "--shared")
    check("2 skill" in run(env, "enable", "--all-parked").stdout, "--all-parked")
    check({"alpha", "beta"} <= set(lock(home)), "everything is back in the lock")

    # ---- over the backend ----
    (_, done) = rpc(env, ("skills.setAgentEnabled", {"name": "alpha", "agent": CLAUDE, "enabled": False}))[0]
    check(done["ok"] and not os.path.lexists(link_of(home, "alpha")), "one agent: %s" % done)
    (_, done) = rpc(env, ("skills.setAgentEnabled", {"names": ["alpha", "beta"], "agent": "@shared", "enabled": False}))[0]
    check(done["ok"] and os.path.isdir(home + "/.agents/.parked/beta"), "many at once: %s" % done)
    (snapshot, _) = rpc(env, ("state.snapshot", {"page": 0, "pageSize": 1000}))[0]
    rows = {r["name"]: r for r in snapshot["result"]["skills"]}
    check(rows["beta"]["parked"] and "off" in rows["beta"]["links"] and not rows["beta"]["problem"], "the row: %s" % rows["beta"])
    (_, done) = rpc(env, ("skills.setAgentEnabled", {"names": ["alpha", "beta"], "agent": "@all", "enabled": True}))[0]
    check(done["ok"] and os.path.islink(link_of(home, "alpha")) and "beta" in lock(home), "all back: %s" % done)
    (_, done) = rpc(env, ("skills.setAgentEnabled", {"agent": "@all", "enabled": True}))[0]
    check(not done["ok"], "no names: %s" % done)

    print("toggles smoke test passed (%s)" % sys.platform)


if __name__ == "__main__":
    try:
        main()
    finally:
        shutil.rmtree(ROOT, ignore_errors=True)
