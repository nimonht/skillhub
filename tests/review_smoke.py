#!/usr/bin/env python3
"""Offline test of reviewing updates and of going back: diff, update --review, what automatic runs leave out, history,
rollback with its hold, restore, and the same over the backend.

usage: python3 tests/review_smoke.py PATH/TO/skillhub[-cli.exe]
"""
import json
import os
import shutil
import sys

from sources_smoke import GITLAB, ROOT, WIN, check, commit, git, lock, make_home, make_upstream, read, rpc, run, serve_hosts, skill, trees, write

GITHUB = "acme/skills"


def text(home, name):
    return read(home + "/.agents/skills/%s/SKILL.md" % name)


def sidecars(home, name):
    return sorted(f for f in os.listdir(home + "/.agents/.backup/" + name) if f.endswith(".json"))


def backups(home, name):
    folder = home + "/.agents/.backup/" + name
    return sorted(f for f in os.listdir(folder) if os.path.isdir(os.path.join(folder, f)))


def main():
    serve_hosts()
    hub = make_upstream("github/acme/skills.git")
    for n in ("alpha", "beta"):
        skill(hub + "/skills/" + n, n, "first\n\nkeep this line")
    commit(hub)
    home, env = make_home("home")
    run(env, "add", GITHUB, "--all")
    first_hash = lock(home)["alpha"]["skillFolderHash"]

    # ---- diff ----
    p = run(env, "diff", "alpha")
    check("no differences" in p.stdout, "nothing changed yet:\n" + p.stdout)
    skill(hub + "/skills/alpha", "alpha", "second\n\nkeep this line")
    write(hub + "/skills/alpha/scripts/run.sh", "#!/bin/sh\necho hi\n")
    if not WIN:
        os.chmod(hub + "/skills/alpha/scripts/run.sh", 0o755)
    commit(hub)
    p = run(env, "diff", "alpha", "--stat")
    check("update available" in p.stdout and "SKILL.md" in p.stdout and "+1 -1" in p.stdout, "the file list:\n" + p.stdout)
    check("scripts/run.sh" in p.stdout and "[script]" in p.stdout and "adds or changes scripts" in p.stdout, "the risk flag:\n" + p.stdout)
    check("+second" not in p.stdout, "--stat has no patch")
    p = run(env, "diff", "alpha")
    check("-first" in p.stdout and "+second" in p.stdout and "+echo hi" in p.stdout, "the patch:\n" + p.stdout)
    check(not os.path.isdir(home + "/cache/skillhub/stage") or not os.listdir(home + "/cache/skillhub/stage"), "diff leaves nothing behind")
    run(env, "diff", "nosuch", expect=1)

    # ---- update --review ----
    p = run(env, "update", "alpha", "--review", expect=2)
    check("--yes" in p.stderr and "first" in text(home, "alpha"), "no terminal, no change:\n" + p.stderr)
    check(not os.listdir(home + "/cache/skillhub/stage"), "a declined review leaves nothing behind")
    p = run(env, "update", "alpha", "--review", "--yes")
    check("+second" in p.stdout and "1 updated, 0 failed" in p.stdout, "review output:\n" + p.stdout)
    check("second" in text(home, "alpha") and os.path.isfile(home + "/.agents/skills/alpha/scripts/run.sh"), "the reviewed update did not arrive")
    second_hash = lock(home)["alpha"]["skillFolderHash"]
    check(second_hash != first_hash and run(env, "check").returncode == 0, "the lock did not follow")
    check(len(sidecars(home, "alpha")) == 1, "a backup comes with its description")
    side = json.loads(read(home + "/.agents/.backup/alpha/" + sidecars(home, "alpha")[0]))
    check(side["reason"] == "update" and side["lockEntry"]["skillFolderHash"] == first_hash and side["treeSha"] == first_hash, "the sidecar: %s" % side)
    check("update" in run(env, "history", "alpha").stdout, "history lists the version")

    # ---- local edits ----
    write(home + "/.agents/skills/beta/notes.md", "my own notes\n")
    p = run(env, "diff", "beta", "--stat")
    check("local edits" in p.stdout and "notes.md" in p.stdout and "update available" not in p.stdout, "edits with no update:\n" + p.stdout)
    skill(hub + "/skills/beta", "beta", "second\n\nkeep this line")
    commit(hub)
    p = run(env, "diff", "beta", "--stat")
    check("update available" in p.stdout and "local edits will be replaced" in p.stdout, "edits before an update:\n" + p.stdout)
    os.remove(home + "/cache/skillhub/sources.json")  # as after an older version, which kept no record of what it placed
    p = run(env, "diff", "beta", "--stat")
    check("update available" in p.stdout and "local edits" not in p.stdout, "with no record nothing is claimed:\n" + p.stdout)
    run(env, "update", "beta")
    write(home + "/.agents/skills/beta/notes.md", "my own notes\n")
    skill(hub + "/skills/beta", "beta", "third\n\nkeep this line")
    commit(hub)
    p = run(env, "diff", "beta", "--stat")
    check("local edits will be replaced" in p.stdout, "an update writes the record again:\n" + p.stdout)

    # ---- what automatic runs leave out ----
    write(hub + "/skills/alpha/scripts/more.py", "print('more')\n")
    commit(hub)
    p = run(env, "update", "--all", "--auto")
    check("left alpha out" in p.stdout and "1 updated, 0 failed" in p.stdout and "1 left out" in p.stdout, "a new script waits:\n" + p.stdout)
    check(not os.path.isfile(home + "/.agents/skills/alpha/scripts/more.py") and "third" in text(home, "beta"), "the wrong skill moved")
    write(env["SKILLHUB_CONFIG"] + "/settings.json", '{"version": 1, "updates": {"autoSkipScripts": false}}')
    p = run(env, "update", "--all", "--auto")
    check("1 updated" in p.stdout and os.path.isfile(home + "/.agents/skills/alpha/scripts/more.py"), "the setting switches it off:\n" + p.stdout)
    os.remove(env["SKILLHUB_CONFIG"] + "/settings.json")

    # ---- rollback and its hold ----
    before = lock(home)["alpha"]["skillFolderHash"]
    p = run(env, "rollback", "alpha")
    check("rolled back" in p.stdout and not os.path.isfile(home + "/.agents/skills/alpha/scripts/more.py"), "the rollback:\n" + p.stdout)
    check(lock(home)["alpha"]["skillFolderHash"] == second_hash, "the lock entry did not come back")
    check("on hold" in run(env, "history", "alpha").stdout, "a rollback holds")
    sides = [json.loads(read(home + "/.agents/.backup/alpha/" + f)) for f in sidecars(home, "alpha")]
    check(any(s["reason"] == "rollback" and s["lockEntry"]["skillFolderHash"] == before for s in sides), "what was rolled back from is kept: %s" % sides)
    run(env, "check", expect=10)
    p = run(env, "update", "--all", "--auto")
    check("on hold" in p.stdout and not os.path.isfile(home + "/.agents/skills/alpha/scripts/more.py"), "the hold must stop the timer:\n" + p.stdout)
    run(env, "update", "alpha")
    check(os.path.isfile(home + "/.agents/skills/alpha/scripts/more.py") and "on hold" not in run(env, "history", "alpha").stdout, "a manual update ends the hold")

    run(env, "rollback", "alpha")
    write(hub + "/skills/alpha/late.md", "upstream moved on\n")
    commit(hub)
    write(env["SKILLHUB_CONFIG"] + "/settings.json", '{"version": 1, "updates": {"autoSkipScripts": false}}')  # the update brings more.py back
    run(env, "update", "--all", "--auto")
    os.remove(env["SKILLHUB_CONFIG"] + "/settings.json")
    check(os.path.isfile(home + "/.agents/skills/alpha/late.md"), "upstream moved past the version rolled back from")
    run(env, "rollback", "alpha", "19990101-000000", expect=1)
    run(env, "rollback", "nosuch", expect=1)

    # ---- hold and release by hand ----
    write(hub + "/skills/beta/late.md", "x\n")
    commit(hub)
    run(env, "hold", "beta")
    check("on hold" in run(env, "update", "--all", "--auto").stdout and not os.path.exists(home + "/.agents/skills/beta/late.md"), "a hold by hand")
    run(env, "release", "beta")
    run(env, "release", "beta", expect=1)
    run(env, "update", "--all", "--auto")
    check(os.path.exists(home + "/.agents/skills/beta/late.md"), "released")
    run(env, "hold", "nosuch", expect=1)

    # ---- an older backup has no description: the files come back, and the next check offers the update ----
    old = home + "/.agents/.backup/beta/20200101-000000"
    skill(old, "beta", "older")
    run(env, "rollback", "beta", "20200101-000000")
    check("older" in text(home, "beta") and lock(home)["beta"]["skillFolderHash"] == "", "an older backup: %s" % lock(home)["beta"])
    run(env, "check", expect=10)
    run(env, "update", "beta")

    # ---- retention: three versions, each with its description ----
    for i in range(4):
        write(hub + "/skills/beta/round.md", "round %d\n" % i)
        commit(hub)
        run(env, "update", "beta")
    check(len(backups(home, "beta")) == 3, "backups: %s" % backups(home, "beta"))
    check({s[:-5] for s in sidecars(home, "beta")} == set(backups(home, "beta")), "a pruned backup keeps no description")

    # ---- restore ----
    entry = lock(home)["beta"]
    run(env, "remove", "beta")
    check("beta" not in lock(home), "removed")
    run(env, "restore", "nosuch", expect=1)
    run(env, "restore", "beta")
    check(lock(home)["beta"] == entry and "round 3" in read(home + "/.agents/skills/beta/round.md"), "restored: %s" % lock(home).get("beta"))
    check(os.path.lexists(home + "/.claude/skills/beta"), "restore links it again")
    run(env, "restore", "beta", expect=1)
    write(home + "/.agents/skills/beta/changed.md", "x\n")
    run(env, "remove", "beta")
    run(env, "restore", "beta")
    check(os.path.isfile(home + "/.agents/skills/beta/changed.md"), "the newest removal comes back")

    # ---- GitLab: the tree record follows ----
    lab = make_upstream("gitlab/group/sub/proj.git")
    skill(lab + "/gamma", "gamma")
    commit(lab)
    run(env, "add", GITLAB, "--all")
    tree = trees(home)["gamma"]["treeSha"]
    skill(lab + "/gamma", "gamma", "new")
    commit(lab)
    run(env, "update", "gamma")
    run(env, "rollback", "gamma")
    check(trees(home)["gamma"]["treeSha"] == tree, "the tree record: %s" % trees(home)["gamma"])
    run(env, "check", expect=10)

    # ---- a local folder, and a live link: nothing to compare ----
    folder = os.path.join(ROOT, "authoring")
    skill(folder + "/linked", "linked")
    run(env, "add", folder, "--skill", "linked", "--link")
    check("live link" in run(env, "diff", "linked", expect=1).stderr, "diff on a live link")
    check("live link" in run(env, "rollback", "linked", expect=1).stderr, "rollback on a live link")

    # ---- over the backend ----
    write(hub + "/skills/alpha/rpc.md", "reviewed\n")
    commit(hub)
    (_, done) = rpc(env, ("skills.diff", {"names": ["alpha"]}))[0]
    review = done["reviews"][0]
    check(done["ok"] and review["available"] and review["stage"], "a review: %s" % review)
    by = {f["path"]: f for f in review["files"]}
    check(by["rpc.md"]["status"] == "added" and by["rpc.md"]["add"] == 1 and "+reviewed" in by["rpc.md"]["patch"], "the file: %s" % by)
    write(hub + "/skills/alpha/after.md", "upstream moved during the review\n")
    commit(hub)
    (_, done) = rpc(env, ("skills.update", {"stages": {"alpha": review["stage"]}}))[0]
    check(done["ok"] and os.path.isfile(home + "/.agents/skills/alpha/rpc.md"), "applying the review: %s" % done)
    check(not os.path.exists(home + "/.agents/skills/alpha/after.md"), "only what was reviewed is applied")
    run(env, "check", expect=10)
    (_, done) = rpc(env, ("skills.update", {"stages": {"alpha": review["stage"]}}))[0]
    check(not done["ok"], "a stage is applied once: %s" % done)
    canary = os.path.join(ROOT, "canary")
    skill(canary + "/new", "alpha", "hostile")
    (_, done) = rpc(env, ("skills.update", {"stages": {"alpha": canary}}))[0]
    check(not done["ok"] and os.path.isdir(canary), "a stage outside the cache: %s" % done)
    (_, done) = rpc(env, ("skills.diff", {"names": ["alpha"]}))[0]
    stage = done["reviews"][0]["stage"]
    write(stage + "/new/SKILL.md", "---\nname: alpha\ndescription: changed behind the review\n---\n")
    (_, done) = rpc(env, ("skills.update", {"stages": {"alpha": stage}}))[0]
    check(not done["ok"] and "changed behind" not in text(home, "alpha"), "a stage changed after the review: %s" % done)

    (reply, _) = rpc(env, ("skills.history", {"name": "alpha"}))[0]
    versions = reply["result"]["versions"]
    check(versions and versions[0]["reason"] == "update" and versions[0]["hasLockEntry"], "history: %s" % reply)
    (_, done) = rpc(env, ("skills.rollback", {"name": "alpha", "stamp": versions[0]["stamp"]}))[0]
    check(done["ok"] and not os.path.isfile(home + "/.agents/skills/alpha/rpc.md"), "rollback: %s" % done)
    (snapshot, _) = rpc(env, ("state.snapshot", {"page": 0, "pageSize": 1000}))[0]
    rows = {r["name"]: r for r in snapshot["result"]["skills"]}
    check(rows["alpha"]["hold"] and rows["alpha"]["versions"] >= 1, "the row: %s" % rows["alpha"])
    (_, done) = rpc(env, ("skills.hold", {"name": "alpha", "on": False}))[0]
    check(done["ok"] and "on hold" not in run(env, "history", "alpha").stdout, "release: %s" % done)
    (_, done) = rpc(env, ("skills.remove", {"name": "alpha"}))[0]
    (_, done) = rpc(env, ("skills.restore", {"name": "alpha"}))[0]
    check(done["ok"] and os.path.isdir(home + "/.agents/skills/alpha"), "restore: %s" % done)

    print("review smoke test passed (%s)" % sys.platform)


if __name__ == "__main__":
    try:
        main()
    finally:
        shutil.rmtree(ROOT, ignore_errors=True)
