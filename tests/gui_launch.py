#!/usr/bin/env python3
"""The window starts and renders on any platform: software-rendered screenshots of the main views, no QML errors.

usage: python3 tests/gui_launch.py PATH/TO/skillhub [OUTPUT_DIR]

Needs no display (QT_QPA_PLATFORM=offscreen; set SKILLHUB_TEST_PLATFORM=cocoa|windows to test a deployed app that
ships only its native platform plugin) and no network. Used by CI on Linux, macOS and Windows.
"""
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading

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


def hostile_text():
    """What a repository wrote is text: the window must not fetch anything because of it, on any page."""
    qml = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "qml")
    for folder, _, names in os.walk(qml):
        for name in names:
            if not name.endswith(".qml"):
                continue
            with open(os.path.join(folder, name), encoding="utf-8") as f:
                text = f.read()
            if len(re.findall(r"(?<![\w.])Text \{", text)) != text.count("textFormat: Text.PlainText"):
                fail("%s: every Text must set textFormat: Text.PlainText, or text from a repository can fetch things" % name)
    hits = []

    class Trap(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            hits.append(self.path)
            self.send_error(404)

        def log_message(self, *args):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Trap)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    trap = "http://127.0.0.1:%d" % server.server_address[1]
    md = ("---\nname: %s\ndescription: <img src=\"%s/%s-description.png\"> <b>bold</b>\n---\n\n"
          "![a picture](%s/%s-markdown.png)\n\n<img src=\"%s/%s-html.png\">\n")

    def write(folder, name):
        os.makedirs(folder)
        with open(folder + "/SKILL.md", "w", newline="\n") as f:
            f.write(md % (name, trap, name, trap, name, trap, name))

    write(home + "/.agents/skills/hostile", "hostile")
    write(home + "/upstream/skills/stranger", "stranger")
    try:
        for name, args in (("hostile-detail", ["--select", "hostile"]), ("hostile-add", ["--mode", "add", "--discover", home + "/upstream"])):
            png = os.path.join(OUT, name + ".png")
            p = subprocess.run([BIN, "gui", "--shot", png, "--theme", "dark", "--size", "1280x800", "--settle", "20", *args],
                               env=env, capture_output=True, text=True, timeout=240)
            if p.returncode != 0:
                fail("%s: exit %d\n%s" % (name, p.returncode, p.stderr))
    finally:
        server.shutdown()
        shutil.rmtree(home + "/.agents/skills/hostile", ignore_errors=True)
    if hits:
        fail("the window fetched what a repository named: %s" % sorted(set(hits)))
    print("ok  hostile text")


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
             ("small", ["--theme", "dark", "--size", "820x600", "--search", "zzzz"]),
             ("restart", ["--theme", "dark", "--size", "1280x800", "--action", "restart", "--settle", "30"])]
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
    shell_env = dict(env, SHELL="/bin/sh")
    shells = [("terminal", None, None)]
    if os.name == "nt":   # on Windows the shell prints the banner itself, so each default shell gets its turn
        cmd = os.environ.get("ComSpec", "C:\\Windows\\System32\\cmd.exe")
        shells = [("terminal", {"shell": cmd, "banner": False}, None), ("terminal-banner", {"shell": cmd, "banner": True}, "cmd")]
        if shutil.which("pwsh"):
            shells.append(("terminal-pwsh", {"shell": shutil.which("pwsh"), "banner": True}, "PowerShell"))
    for name, settings, shell in shells:
        dump = os.path.join(OUT, name + ".txt")
        if settings:
            config = env.get("SKILLHUB_CONFIG") or os.path.join(home, "AppData", "Roaming", "skillhub")
            os.makedirs(config, exist_ok=True)
            with open(os.path.join(config, "settings.json"), "w") as f:
                json.dump({"version": 1, "terminal": settings}, f)
        p = subprocess.run([BIN, "gui", "--shot", os.path.join(OUT, name + ".png"), "--theme", "dark", "--size", "1280x800", "--layout", "dock=340", "--mode", "terminal",
                            "--terminal-run", "echo pty-check", "--terminal-dump", dump, "--settle", "60" if os.name == "nt" else "20"],
                           env=shell_env, capture_output=True, text=True, timeout=240)
        if p.returncode != 0:
            fail("%s: exit %d\n%s" % (name, p.returncode, p.stderr))
        with open(dump, encoding="utf-8") as f:
            screen = f.read()
        if screen.count("pty-check") < 2:   # the command as typed, and its answer
            fail("%s: the shell did not answer through the terminal:\n%s\n--- stdout\n%s\n--- stderr\n%s" % (name, screen, p.stdout, p.stderr))
        if shell and not re.search(r"Shell\s+%s\s" % shell, screen):   # the banner, naming the shell it runs in
            fail("%s: no banner naming %s in the terminal:\n%s\n--- stdout\n%s\n--- stderr\n%s" % (name, shell, screen, p.stdout, p.stderr))
        print("ok  " + name)
    # a closed terminal gives its number back, and the tabs that stay open keep theirs
    tabs = os.path.join(OUT, "terminal-tabs.txt")
    p = subprocess.run([BIN, "gui", "--shot", os.path.join(OUT, "terminal-tabs.png"), "--theme", "dark", "--size", "1280x800", "--layout", "dock=340", "--mode", "terminal",
                        "--terminal-tabs", tabs, "--settle", "10"], env=shell_env, capture_output=True, text=True, timeout=240)
    if p.returncode != 0:
        fail("terminal tabs: exit %d\n%s" % (p.returncode, p.stderr))
    with open(tabs, encoding="utf-8") as f:
        names = f.read().splitlines()
    wanted = ["Terminal 1, Terminal 2, Terminal 3", "Terminal 1, Terminal 3", "Terminal 1, Terminal 3, Terminal 2", "Terminal 1"]
    if names != wanted:
        fail("terminal tabs: wanted %s, got %s\n--- stderr\n%s" % (wanted, names, p.stderr))
    print("ok  terminal tabs")
    if os.name != "nt":
        # a program that sends terminal queries and reads none of the answers: the window keeps running, whatever the shell does next
        flood = "\"%s\" -c \"import sys; sys.stdout.write((chr(27) + '[c') * 400000)\"" % sys.executable
        try:
            p = subprocess.run([BIN, "gui", "--shot", os.path.join(OUT, "flood.png"), "--theme", "dark", "--size", "1280x800", "--layout", "dock=340", "--mode", "terminal",
                                "--terminal-run", flood, "--settle", "10"], env=shell_env, capture_output=True, text=True, timeout=90)
        except subprocess.TimeoutExpired:
            fail("the window hung when a program flooded the terminal with queries")
        if p.returncode != 0:
            fail("terminal flood: exit %d\n%s" % (p.returncode, p.stderr))
        print("ok  terminal flood")
    hostile_text()
    print("gui launch test passed (screenshots in %s)" % OUT)
finally:
    shutil.rmtree(home, ignore_errors=True)
