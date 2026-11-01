#!/usr/bin/env python3
"""Offline test of what Skill Hub keeps besides skills: clones that do not grow, the cache limit, the age of loose backups,
that conflicts are never removed on their own, `skillhub clean`, and the same over the backend.

usage: python3 tests/storage_smoke.py PATH/TO/skillhub[-cli.exe]
"""
import json
import os
import shutil
import sys
import time

from sources_smoke import ROOT, check, commit, listed, make_home, make_upstream, read, rpc, run, serve_hosts, skill, write

MB = 1024 * 1024


def size(path):
    return sum(os.path.getsize(os.path.join(d, f)) for d, _, names in os.walk(path) for f in names)


def blob(path, n=2 * MB):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(os.urandom(n))


def settings(env, **storage):
    write(env["SKILLHUB_CONFIG"] + "/settings.json", json.dumps({"version": 1, "storage": storage}))


def backup(home, folder, name, text="kept\n"):
    path = "%s/.agents/.backup/%s/%s" % (home, folder, name)
    write(path + "/SKILL.md", text)
    return path


def everything_in_backups(home):
    return sorted(os.path.join(d, f) for d, _, names in os.walk(home + "/.agents/.backup") for f in names)


def main():
    serve_hosts()
    hub = make_upstream("github/acme/skills.git")
    skill(hub + "/skills/big", "big")
    blob(hub + "/skills/big/data.bin")
    commit(hub)
    home, env = make_home("home")
    run(env, "add", "acme/skills", "--all")
    clone = home + "/cache/skillhub/repos/acme__skills"
    first = size(clone)

    # ---- a clone stays the size of one version, however many updates pass through it ----
    for _ in range(5):
        blob(hub + "/skills/big/data.bin")
        commit(hub)
        run(env, "update", "--all")
    check(size(clone) < first * 2, "a clone grew with its updates: %d MB, from %d MB" % (size(clone) // MB, first // MB))
    check(len(os.listdir(home + "/.agents/.backup/big")) <= 6, "versions are kept in number")  # three, with their sidecars
    check(run(env, "check").returncode == 0, "a compacted clone still works")

    # ---- the cache limit: the clone used longest ago goes first, and comes back by itself ----
    other = make_upstream("github/acme/other.git")
    skill(other + "/skills/small", "small")
    blob(other + "/skills/small/data.bin")
    commit(other)
    time.sleep(0.05)
    settings(env, cacheMaxMB=3)
    run(env, "add", "acme/other", "--all")
    check(not os.path.exists(clone) and os.path.isdir(home + "/cache/skillhub/repos/acme__other"), "the older clone goes first")
    check(os.path.isfile(home + "/.agents/skills/big/data.bin"), "the limit touches clones, never skills")
    run(env, "check")
    check(os.path.isdir(clone), "a clone comes back when it is needed")
    settings(env)

    # ---- loose backups age by the date in their name; what cannot be dated, and conflicts, stay ----
    stamp = time.strftime("%Y%m%d-%H%M%S")
    old = backup(home, "_removed", "gone-20200101-000000")
    write(old + ".json", "{}\n")
    recent = backup(home, "_removed", "fresh-" + stamp)
    os.utime(recent + "/SKILL.md", (1577836800, 1577836800))
    os.utime(recent, (1577836800, 1577836800))  # moved here today from a folder last changed long ago: its time says nothing
    dupe = backup(home, "_dupes", "claude-code-twin-20200101-000000-2")
    replaced = backup(home, ".parked-replaced", "swapped-20200101-000000")
    undated = backup(home, "_removed", "no-date-at-all")
    conflict = backup(home, "_conflicts", "claude-code-mine-20200101-000000", "my own edits\n")
    blob(hub + "/skills/big/data.bin")
    commit(hub)
    run(env, "update", "--all")
    check(not os.path.exists(old) and not os.path.exists(old + ".json"), "an old removed skill stays, or its sidecar does")
    check(not os.path.exists(dupe) and not os.path.exists(replaced), "old duplicates and replaced copies stay")
    check(os.path.exists(recent), "a backup was aged by its folder's time, not by when it was made")
    check(os.path.exists(undated), "a backup without a date was removed on a guess")
    check(read(conflict + "/SKILL.md") == "my own edits\n", "a conflict was removed without being asked")

    # ---- skillhub clean ----
    run(env, "update", "--all")  # nothing to update; the versions stay
    versions = sorted(n for n in os.listdir(home + "/.agents/.backup/big") if not n.endswith(".json"))
    check(len(versions) >= 2, "the test needs older versions: %s" % versions)
    before = everything_in_backups(home)
    cache = size(home + "/cache/skillhub/repos")
    p = run(env, "clean", "--dry-run")
    check("would free" in p.stdout and everything_in_backups(home) == before and size(home + "/cache/skillhub/repos") == cache, "--dry-run:\n" + p.stdout)
    run(env, "clean", expect=2)  # no terminal, no --yes
    run(env, "clean", "--conflict", "--yes", expect=2)  # a typo must not become "the default set"
    check(everything_in_backups(home) == before, "a refused clean changed something")

    run(env, "clean", "--yes")
    left = [n for n in os.listdir(home + "/.agents/.backup/big") if not n.endswith(".json")]
    check(left == [versions[-1]], "the newest version of a skill is kept: %s" % left)
    check(read(conflict + "/SKILL.md") == "my own edits\n", "plain clean took a conflict: they go only when named")
    check(not os.path.exists(undated) and not os.path.exists(recent), "plain clean leaves removed skills")
    check(not os.path.exists(clone), "plain clean leaves the cache")
    check({"big", "small"} <= set(listed(env)), "clean touched skills")
    run(env, "clean", "--conflicts", "--yes")
    check(not os.path.exists(conflict), "--conflicts")
    check(run(env, "check").returncode == 0, "everything comes back after a clean")

    # ---- over the backend ----
    backup(home, "_conflicts", "claude-code-again-20200101-000000")
    (_, done) = rpc(env, ("paths.sizes", {}))[0]
    rows = {r["key"]: r for r in done["sizes"]}
    check({"skills", "cache", "versions", "removed", "conflicts", "latest"} <= set(rows), "paths.sizes: %s" % sorted(rows))
    check(rows["cache"]["bytes"] > 0 and rows["conflicts"]["bytes"] > 0, "the measure: %s" % rows)
    (_, done) = rpc(env, ("storage.clean", {"cache": True}))[0]
    check(done["ok"] and done["summary"]["freed"] >= rows["cache"]["bytes"], "storage.clean: %s" % done)
    check(os.path.isdir(home + "/.agents/.backup/_conflicts/claude-code-again-20200101-000000"), "only what was asked for goes")

    print("storage smoke test passed (%s)" % sys.platform)


if __name__ == "__main__":
    try:
        main()
    finally:
        shutil.rmtree(ROOT, ignore_errors=True)
