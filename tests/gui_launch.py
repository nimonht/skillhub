#!/usr/bin/env python3
"""The window starts and renders on any platform: software-rendered screenshots of the main views, no QML errors.

usage: python3 tests/gui_launch.py PATH/TO/skillhub [OUTPUT_DIR]

Needs no display (QT_QPA_PLATFORM=offscreen; set SKILLHUB_TEST_PLATFORM=cocoa|windows to test a deployed app that
ships only its native platform plugin) and no network. Used by CI on Linux, macOS and Windows.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

BIN = os.path.abspath(sys.argv[1])
OUT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else tempfile.mkdtemp(prefix="skillhub-shots-")
os.makedirs(OUT, exist_ok=True)

home = os.path.realpath(tempfile.mkdtemp(prefix="skillhub-gui-"))
env = dict(os.environ, HOME=home, USERPROFILE=home, SKILLHUB_HOME=home, XDG_CONFIG_HOME=home + "/.config", XDG_CACHE_HOME=home + "/.cache",
           XDG_STATE_HOME=home + "/.local/state", XDG_DATA_HOME=home + "/.local/share",
           SKILLHUB_CACHE=home + "/cache", SKILLHUB_CONFIG=home + "/config", SKILLHUB_BIN_PATH=home + "/none",
           QT_QPA_PLATFORM=os.environ.get("SKILLHUB_TEST_PLATFORM", "offscreen"), QT_QUICK_BACKEND="software", SKILLHUB_REDUCE_MOTION="1",
           QT_FORCE_STDERR_LOGGING="1")


def fail(msg):
    print("FAIL:", msg, file=sys.stderr)
    sys.exit(1)


try:
    os.makedirs(home + "/.claude/skills")
    for n in ("alpha", "beta", "gamma"):
        os.makedirs("%s/.agents/skills/%s" % (home, n))
        with open("%s/.agents/skills/%s/SKILL.md" % (home, n), "w", newline="\n") as f:
            f.write("---\nname: %s\ndescription: demo skill %s\n---\n\n# %s\n\nSome **markdown** with `code`.\n" % (n, n, n))
    subprocess.run([BIN, "adopt"], env=env, check=True, capture_output=True, timeout=120)

    shots = [("dark", ["--theme", "dark", "--size", "1280x800", "--select", "alpha"]),
             ("light", ["--theme", "light", "--size", "1000x640", "--select", "alpha"]),
             ("agent", ["--theme", "dark", "--size", "1280x800", "--agent", "claude-code"]),
             ("agents", ["--theme", "light", "--size", "1280x800", "--mode", "providers"]),
             ("transfer", ["--theme", "dark", "--size", "1280x800", "--mode", "transfer"]),
             ("small", ["--theme", "dark", "--size", "820x600", "--search", "zzzz"])]
    for name, args in shots:
        png = os.path.join(OUT, name + ".png")
        p = subprocess.run([BIN, "gui", "--shot", png, *args], env=env, capture_output=True, text=True, timeout=240)
        if p.returncode != 0:
            fail("%s: exit %d\n%s" % (name, p.returncode, p.stderr))
        bad = [l for l in p.stderr.splitlines() if re.search(r"\.qml:\d+|TypeError|ReferenceError|Binding loop|is not installed|module .* is not", l)]
        if bad:
            fail("%s: QML reported errors:\n%s" % (name, "\n".join(bad)))
        if not os.path.isfile(png) or os.path.getsize(png) < 8000:
            fail("%s: screenshot missing or nearly empty" % name)
        print("ok  " + name)
    # the terminal on every system: a real shell on a real pty (ConPTY on Windows) answers through the emulator
    dump = os.path.join(OUT, "terminal.txt")
    shell_env = dict(env, SHELL="/bin/sh")
    p = subprocess.run([BIN, "gui", "--shot", os.path.join(OUT, "terminal.png"), "--theme", "dark", "--size", "1280x800", "--layout", "dock=340", "--mode", "terminal",
                        "--terminal-run", "echo pty-check", "--terminal-dump", dump, "--settle", "60" if os.name == "nt" else "20"],
                       env=shell_env, capture_output=True, text=True, timeout=240)
    if p.returncode != 0:
        fail("terminal: exit %d\n%s" % (p.returncode, p.stderr))
    with open(dump, encoding="utf-8") as f:
        screen = f.read()
    if screen.count("pty-check") < 2:   # the command as typed, and its answer
        fail("the shell did not answer through the terminal:\n" + screen)
    print("ok  terminal")
    print("gui launch test passed (screenshots in %s)" % OUT)
finally:
    shutil.rmtree(home, ignore_errors=True)
