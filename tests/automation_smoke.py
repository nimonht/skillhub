#!/usr/bin/env python3
"""Offline test of the daily update and the stray watcher: what each of the three backends writes and runs, in a dry run, so
all of them are tested on any system; the environment scheduled runs get; and the resident watcher, for real.

usage: python3 tests/automation_smoke.py PATH/TO/skillhub[-cli.exe]
"""
import json
import os
import plistlib
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

from sources_smoke import BIN, ROOT, check, make_home, read, run, skill

TASK = "{http://schemas.microsoft.com/windows/2004/02/mit/task}"


def dry(env, backend):
    return dict(env, SKILLHUB_AUTOMATION_DRYRUN=backend)


def launchd(home, env):
    env = dry(env, "launchd")
    p = run(env, "autoupdate", "on")
    path = home + "/Library/LaunchAgents/io.github.nimonht.skillhub.update.plist"
    with open(path, "rb") as f:
        plist = plistlib.load(f)
    check(plist["Label"] == "io.github.nimonht.skillhub.update" and plist["ProgramArguments"][1:] == ["update", "--all", "--auto", "--notify"], "the plist: %s" % plist)
    check(plist["ProgramArguments"][0] == BIN or "skillhub" in plist["ProgramArguments"][0], "it runs the command line: %s" % plist)
    check(9 <= plist["StartCalendarInterval"]["Hour"] <= 20 and plist["Nice"] == 10, "the schedule: %s" % plist)
    check("Library/Logs/skillhub" in plist["StandardOutPath"].replace("\\", "/"), "the log: %s" % plist)
    check("launchctl bootstrap gui/" in p.stdout and path.replace("\\", "/") in p.stdout.replace("\\", "/"), "loaded:\n" + p.stdout)
    check("enabled" in run(env, "autoupdate", "status").stdout, "status")
    again = run(env, "autoupdate", "on")
    with open(path, "rb") as f:
        check(plistlib.load(f)["StartCalendarInterval"] == plist["StartCalendarInterval"], "the time is chosen once")
    check("launchctl bootout gui/" in again.stdout, "a loaded agent is replaced")
    p = run(env, "watch", "on")
    watcher = home + "/Library/LaunchAgents/io.github.nimonht.skillhub.adopt.plist"
    with open(watcher, "rb") as f:
        plist = plistlib.load(f)
    check(plist["ThrottleInterval"] == 10 and plist["ProgramArguments"][1:] == ["adopt", "--notify"], "the watcher: %s" % plist)
    check(any(w.replace("\\", "/").endswith("/.claude/skills") for w in plist["WatchPaths"]), "what it watches: %s" % plist["WatchPaths"])
    p = run(env, "providers", "add", "Mine", home + "/mine/skills")
    with open(watcher, "rb") as f:
        check(any("mine/skills" in w.replace("\\", "/") for w in plistlib.load(f)["WatchPaths"]), "a new agent reaches the watcher: " + p.stdout)
    run(env, "providers", "remove", "custom-mine")
    run(env, "uninstall", "--dry-run")
    p = run(env, "autoupdate", "off")
    check("launchctl bootout gui/" in p.stdout and not os.path.exists(path), "removed:\n" + p.stdout)
    check("not installed" in run(env, "autoupdate", "status").stdout, "status after off")
    run(env, "watch", "off")
    check(not os.path.exists(watcher), "the watcher's file")


def schtasks(home, env):
    env = dry(env, "schtasks")
    p = run(env, "autoupdate", "on")
    check("schtasks /Create /XML" in p.stdout and "SkillHub\\Daily update" in p.stdout and "/F" in p.stdout, "created:\n" + p.stdout)
    path = env["SKILLHUB_CACHE"] + "/automation/update.xml"
    with open(path, "rb") as f:
        data = f.read()
    check(data[:2] == b"\xff\xfe", "UTF-16, as schtasks reads it")
    task = ET.fromstring(data.decode("utf-16"))
    check(task.find(TASK + "Triggers/" + TASK + "CalendarTrigger/" + TASK + "RandomDelay").text == "PT30M", "a random delay")
    check(task.find(TASK + "Settings/" + TASK + "StartWhenAvailable").text == "true", "a missed run is made up for")
    principal = task.find(TASK + "Principals/" + TASK + "Principal")
    check(principal.find(TASK + "LogonType").text == "InteractiveToken" and principal.find(TASK + "RunLevel").text == "LeastPrivilege", "the principal")
    command = task.find(TASK + "Actions/" + TASK + "Exec/" + TASK + "Command").text
    check(command.endswith("skillhubw.exe"), "the windowless program: %s" % command)
    check(task.find(TASK + "Actions/" + TASK + "Exec/" + TASK + "Arguments").text == "update --all --auto --notify", "its arguments")
    check("enabled" in run(env, "autoupdate", "status").stdout, "status")
    p = run(env, "watch", "on")
    watcher = env["SKILLHUB_CACHE"] + "/automation/watcher.xml"
    with open(watcher, "rb") as f:
        task = ET.fromstring(f.read().decode("utf-16"))
    check(task.find(TASK + "Triggers/" + TASK + "LogonTrigger") is not None, "a logon task")
    check(task.find(TASK + "Actions/" + TASK + "Exec/" + TASK + "Arguments").text == "watch --run", "the resident watcher")
    check(task.find(TASK + "Settings/" + TASK + "RestartOnFailure") is not None, "it restarts")
    check("schtasks /Run /TN SkillHub\\Stray watcher" in p.stdout, "it starts right away:\n" + p.stdout)
    p = run(env, "watch", "off")
    check("schtasks /End /TN SkillHub\\Stray watcher" in p.stdout and "schtasks /Delete /TN SkillHub\\Stray watcher /F" in p.stdout, "removed:\n" + p.stdout)
    run(env, "autoupdate", "off")
    check("not installed" in run(env, "autoupdate", "status").stdout, "status after off")


def systemd(home, env):
    env = dry(env, "systemd")
    p = run(env, "autoupdate", "on")
    check("systemctl --user enable --now skillhub-update.timer" in p.stdout, "enabled:\n" + p.stdout)
    service = read(home + "/.config/systemd/user/skillhub-update.service")
    check("update --all --auto --notify" in service and "Nice=10" in service, service)
    check("OnCalendar=daily" in read(home + "/.config/systemd/user/skillhub-update.timer"), "the timer")
    run(env, "watch", "on")
    check("PathChanged=" in read(home + "/.config/systemd/user/skillhub-adopt.path"), "the path unit")
    run(env, "autoupdate", "off")
    run(env, "watch", "off")
    check(not os.path.exists(home + "/.config/systemd/user/skillhub-adopt.path"), "removed")


def scheduled_env(home, env):
    """A scheduled run lacks the login environment: what was recorded when automation was switched on fills in."""
    agent = os.path.join(ROOT, "claude-elsewhere")
    os.makedirs(agent + "/skills")
    with_var = dict(dry(env, "launchd"), CLAUDE_CONFIG_DIR=agent)
    run(with_var, "autoupdate", "on")
    state = json.loads(read(env["SKILLHUB_CONFIG"] + "/state.json"))
    check(state["automationEnv"]["CLAUDE_CONFIG_DIR"] == agent, "recorded: %s" % state.get("automationEnv"))
    skill(agent + "/skills/stray", "stray")
    plain = {k: v for k, v in env.items() if k != "CLAUDE_CONFIG_DIR"}
    run(plain, "adopt", "--notify")
    check(os.path.isfile(home + "/.agents/skills/stray/SKILL.md"), "a scheduled run must see the same agent folder")
    run(plain, "remove", "stray")
    run(with_var, "autoupdate", "off")
    shutil.rmtree(agent)
    state = json.loads(read(env["SKILLHUB_CONFIG"] + "/state.json"))
    del state["automationEnv"]
    with open(env["SKILLHUB_CONFIG"] + "/state.json", "w") as f:
        json.dump(state, f)


def resident(home, env):
    """The watcher that stays: a stray dropped into an agent's folder is pulled in, and a new agent's folder is watched too."""
    p = subprocess.Popen([BIN, "watch", "--run"], env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        time.sleep(1.5)
        check(p.poll() is None, "the watcher ended: %s" % (p.stderr.read() if p.poll() is not None else ""))
        skill(home + "/.claude/skills/dropped", "dropped")
        target = home + "/.agents/skills/dropped/SKILL.md"
        deadline = time.time() + 30
        while time.time() < deadline and not os.path.isfile(target):
            time.sleep(0.25)
        check(os.path.isfile(target), "the stray was not adopted")
        check(p.poll() is None, "the watcher ended after its first pass")
    finally:
        p.terminate()
        p.wait(timeout=10)
        p.stderr.close()


def main():
    home, env = make_home("home")
    run(env, "adopt")
    launchd(home, env)
    schtasks(home, env)
    systemd(home, env)
    scheduled_env(home, env)
    resident(home, env)
    print("automation smoke test passed (%s)" % sys.platform)


if __name__ == "__main__":
    try:
        main()
    finally:
        shutil.rmtree(ROOT, ignore_errors=True)
