#!/usr/bin/env python3
"""One greeting in a new terminal: the Skill Hub banner or the shell's own fetch tool, never both.

usage: python3 tests/greeting_smoke.py PATH/TO/skillhub

Starts the window without a display, on a real pty, with a throw-away $HOME whose startup files run a stand-in `fastfetch`. Shells that
are not installed are skipped. The user's own startup files must come out of each run unchanged.
"""
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile

BIN = os.path.abspath(sys.argv[1])
MARK = "OWN-FETCH-RAN"


def fail(msg):
    print("FAIL: " + msg, file=sys.stderr)
    sys.exit(1)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def prepare(with_fetch):
    home = os.path.realpath(tempfile.mkdtemp(prefix="skillhub-greeting-"))
    tools = home + "/tools"
    write(tools + "/fastfetch", "#!/bin/sh\necho %s\n" % MARK)
    os.chmod(tools + "/fastfetch", stat.S_IRWXU)
    start = 'export PATH="%s:$PATH"\n' % tools
    call = "fastfetch\n" if with_fetch else ""
    write(home + "/.zshenv", start)
    write(home + "/.zshrc", "PS1='ready> '\n" + call)
    write(home + "/.bash_profile", start + "PS1='ready> '\n" + call)
    write(home + "/.config/fish/config.fish", 'set -gx PATH "%s" $PATH\nfunction fish_greeting\n' % tools + "    " + call + "end\n")
    return home


def startup_files(home):
    out = {}
    for name in (".zshenv", ".zshrc", ".bash_profile", ".config/fish/config.fish"):
        with open(os.path.join(home, name), encoding="utf-8") as f:
            out[name] = f.read()
    return out


def run(shell, home, greeting, command):
    with open(home + "/settings-dir-marker", "w"):
        pass
    config = home + "/config"
    settings = {"version": 1, "terminal": {"shell": shell}}
    if greeting:
        settings["terminal"]["greeting"] = greeting
    write(config + "/settings.json", json.dumps(settings))
    env = dict(os.environ, HOME=home, SKILLHUB_HOME=home, XDG_CONFIG_HOME=home + "/.config", XDG_CACHE_HOME=home + "/.cache", XDG_STATE_HOME=home + "/.local/state",
               XDG_DATA_HOME=home + "/.local/share", XDG_RUNTIME_DIR=home + "/run", SKILLHUB_CACHE=home + "/cache", SKILLHUB_CONFIG=config, SKILLHUB_BIN_PATH=home + "/none",
               QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software", SKILLHUB_REDUCE_MOTION="1", QT_FORCE_STDERR_LOGGING="1", SHELL=shell)
    env.pop("ZDOTDIR", None)
    os.makedirs(home + "/run", mode=0o700, exist_ok=True)
    dump = home + "/screen.txt"
    p = subprocess.run([BIN, "gui", "--shot", home + "/shot.png", "--theme", "dark", "--size", "1280x800", "--layout", "dock=460", "--mode", "terminal",
                        "--terminal-run", command, "--terminal-dump", dump, "--settle", "20"], env=env, capture_output=True, text=True, timeout=240)
    if p.returncode != 0:
        fail("%s, %s: exit %d\n%s" % (shell, greeting, p.returncode, p.stderr))
    with open(dump, encoding="utf-8") as f:
        return f.read()


def banner_shown(screen, shell):
    return re.search(r"Shell\s+%s\s" % os.path.basename(shell), screen) is not None


shells = [s for s in (shutil.which(n) for n in ("zsh", "bash", "fish")) if s]
if not shells:
    print("skipped: none of zsh, bash, fish is installed")
    sys.exit(0)

for shell in shells:
    name = os.path.basename(shell)
    home = prepare(True)
    before = startup_files(home)

    screen = run(shell, home, None, "echo pty-check")
    if screen.count(MARK) != 1 or banner_shown(screen, shell):
        fail("%s, automatic: the shell's own fetch alone was expected:\n%s" % (name, screen))
    screen = run(shell, home, "shell", "echo pty-check")
    if screen.count(MARK) != 1 or banner_shown(screen, shell):
        fail("%s, my shell: the shell's own fetch alone was expected:\n%s" % (name, screen))

    screen = run(shell, home, "skillhub", "fastfetch")
    if not banner_shown(screen, shell):
        fail("%s, Skill Hub: no banner:\n%s" % (name, screen))
    if screen.count(MARK) != 1:  # quiet while starting, and working again when typed
        fail("%s, Skill Hub: the fetch tool must stay quiet at the start and answer when typed:\n%s" % (name, screen))

    screen = run(shell, home, "skillhub", "type fastfetch; echo pty-check")
    if screen.count("pty-check") < 2 or re.search(r"fastfetch is a (shell )?function", screen):
        fail("%s, Skill Hub: the stand-in function stayed behind:\n%s" % (name, screen))
    if startup_files(home) != before:
        fail("%s: the startup files were changed" % name)
    shutil.rmtree(home, ignore_errors=True)

    home = prepare(False)
    screen = run(shell, home, None, "echo pty-check")
    if not banner_shown(screen, shell) or MARK in screen:
        fail("%s, automatic with no fetch in the startup files: the banner was expected:\n%s" % (name, screen))
    shutil.rmtree(home, ignore_errors=True)
    print("ok  " + name)

print("greeting smoke test passed")
