#!/usr/bin/env python3
"""Skill Hub updating itself: what it installs, and above all what it refuses.

usage: python3 tests/selfupdate_smoke.py PATH/TO/skillhub-update-test [PATH/TO/THE/WINDOW]

That program calls itself 1.0.0-beta1 and trusts the test key below (RFC 8032, section 7.1, TEST 1), never the project's key. A local
git repository plays the tags and a local web server plays the release, so no network is needed. Used by CI on Linux, macOS and Windows.
With the window's program, it also presses the window's update button (Linux and Windows).
"""
import hashlib
import http.server
import json
import os
import platform
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import threading
import time

BIN = os.path.abspath(sys.argv[1])
WINDOW = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
SEED = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
OTHER_SEED = bytes.fromhex("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb")
PUBLIC = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
NEW = "1.0.0-beta2"

# ---- Ed25519 signing, RFC 8032 section 6: the standard library has none, and the macOS openssl cannot do it ----
P = 2**255 - 19
Q = 2**252 + 27742317777372353535851937790883648493
D = -121665 * pow(121666, -1, P) % P
ROOT_M1 = pow(2, (P - 1) // 4, P)


def point_add(a, b):
    x, y = (a[1] - a[0]) * (b[1] - b[0]) % P, (a[1] + a[0]) * (b[1] + b[0]) % P
    c, d = 2 * a[3] * b[3] * D % P, 2 * a[2] * b[2] % P
    e, f, g, h = y - x, d - c, d + c, y + x
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def point_mul(s, a):
    result = (0, 1, 1, 0)
    while s:
        if s & 1:
            result = point_add(result, a)
        a = point_add(a, a)
        s >>= 1
    return result


def recover_x(y):
    x2 = (y * y - 1) * pow(D * y * y + 1, -1, P) % P
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P:
        x = x * ROOT_M1 % P
    return P - x if x & 1 else x


BASE_Y = 4 * pow(5, -1, P) % P
BASE_X = recover_x(BASE_Y)
BASE = (BASE_X, BASE_Y, 1, BASE_X * BASE_Y % P)


def compress(a):
    z = pow(a[2], -1, P)
    x, y = a[0] * z % P, a[1] * z % P
    return (y | (x & 1) << 255).to_bytes(32, "little")


def sign(seed, message):
    h = hashlib.sha512(seed).digest()
    a = (int.from_bytes(h[:32], "little") & ((1 << 254) - 8)) | 1 << 254
    public = compress(point_mul(a, BASE))
    r = int.from_bytes(hashlib.sha512(h[32:] + message).digest(), "little") % Q
    big_r = compress(point_mul(r, BASE))
    k = int.from_bytes(hashlib.sha512(big_r + public + message).digest(), "little") % Q
    return big_r + ((r + k * a) % Q).to_bytes(32, "little"), public


# ---- the stage ----
def fail(msg):
    print("FAIL:", msg, file=sys.stderr)
    sys.exit(1)


if sign(SEED, b"")[1].hex() != PUBLIC:
    fail("the test key and the signer disagree")

top = os.path.realpath(tempfile.mkdtemp(prefix="skillhub-selfupdate-"))
site = os.path.join(top, "site")
env = dict(os.environ, HOME=top, USERPROFILE=top, SKILLHUB_HOME=top, SKILLHUB_CACHE=top + "/cache", SKILLHUB_CONFIG=top + "/config", SKILLHUB_STATE=top + "/state",
           XDG_CONFIG_HOME=top + "/.config", XDG_CACHE_HOME=top + "/.cache", XDG_STATE_HOME=top + "/.state")


class Quiet(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kw):
        super().__init__(*args, directory=site, **kw)

    def log_message(self, *args):
        pass


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
threading.Thread(target=server.serve_forever, daemon=True).start()
env["SKILLHUB_UPDATE_BASE"] = "http://127.0.0.1:%d" % server.server_address[1]


def git(*args, cwd):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *args], cwd=cwd, check=True, capture_output=True)


def tags_repo(name, tags):
    path = os.path.join(top, name)
    os.makedirs(path)
    git("init", "-q", cwd=path)
    git("commit", "-q", "--allow-empty", "-m", "x", cwd=path)
    for tag in tags:
        git("tag", tag, cwd=path)
    return path


def executable(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        f.write(text)
    os.chmod(path, 0o755)


def says(version):
    return "#!/bin/sh\necho 'skillhub %s'\n" % version


def machine():
    m = platform.machine().lower()
    return "aarch64" if m in ("aarch64", "arm64") else "x86_64"


def linux_asset(says_version):
    folder = os.path.join(top, "build-asset")
    shutil.rmtree(folder, ignore_errors=True)
    executable(os.path.join(folder, "bin", "skillhub"), says(says_version))
    executable(os.path.join(folder, "bin", "skillhub-gui"), '#!/bin/sh\necho started > "$HOME/new-window-started"\n')
    name = "skillhub-%s-linux-%s.tar.gz" % (NEW, machine())
    path = os.path.join(top, name)
    with tarfile.open(path, "w:gz") as t:
        t.add(folder, arcname="skillhub-" + NEW)
    return name, path


def macos_asset(says_version):
    folder = os.path.join(top, "build-asset")
    shutil.rmtree(folder, ignore_errors=True)
    executable(os.path.join(folder, "Skill Hub.app", "Contents", "MacOS", "skillhub"), says(says_version))
    name = "SkillHub-%s-macos-universal.dmg" % NEW
    path = os.path.join(top, name)
    if os.path.exists(path):
        os.remove(path)
    subprocess.run(["hdiutil", "create", "-volname", "t", "-srcfolder", folder, "-ov", "-format", "UDZO", path], check=True, capture_output=True)
    return name, path


def windows_asset(says_version):
    # a harmless real program in the installer's place: it runs, ignores what it is told, and ends
    name = "SkillHub-%s-windows-x64-setup.exe" % NEW
    path = os.path.join(top, name)
    shutil.copyfile(os.path.join(os.environ.get("SystemRoot", "C:\\Windows"), "System32", "whoami.exe"), path)
    return name, path


make_asset = windows_asset if os.name == "nt" else macos_asset if sys.platform == "darwin" else linux_asset

# the install under test, in the shape each kind has
install = os.path.join(top, "install")
if os.name == "nt":
    program = os.path.join(install, "skillhub-cli.exe")
    os.makedirs(install)
    shutil.copyfile(BIN, program)
    with open(os.path.join(install, "unins000.exe"), "wb"):
        pass
elif sys.platform == "darwin":
    program = os.path.join(install, "Skill Hub.app", "Contents", "MacOS", "skillhub")
    os.makedirs(os.path.dirname(program))
    shutil.copyfile(BIN, program)
    os.chmod(program, 0o755)
else:
    program = os.path.join(install, "1.0.0-beta1", "bin", "skillhub")
    os.makedirs(os.path.dirname(program))
    shutil.copyfile(BIN, program)
    os.chmod(program, 0o755)
    os.symlink("1.0.0-beta1", os.path.join(install, "current"))


def publish(asset=None, sums=None, signature="good", content=None, version=NEW):
    """The release of `version` as the web server shows it."""
    shutil.rmtree(site, ignore_errors=True)
    folder = os.path.join(site, "v" + version)
    os.makedirs(folder)
    name, path = asset
    with open(path, "rb") as f:
        data = f.read()
    with open(os.path.join(folder, name), "wb") as f:
        f.write(data if content is None else content)
    text = sums if sums is not None else "%s  %s\n" % (hashlib.sha256(data).hexdigest(), name)
    with open(os.path.join(folder, "SHA256SUMS"), "w", newline="\n") as f:
        f.write(text)
    if signature == "good":
        sig = sign(SEED, text.encode())[0]
    elif signature == "other key":
        sig = sign(OTHER_SEED, text.encode())[0]
    elif signature == "changed":
        sig = bytearray(sign(SEED, text.encode())[0])
        sig[3] ^= 1
    else:
        return
    with open(os.path.join(folder, "SHA256SUMS.sig"), "wb") as f:
        f.write(bytes(sig))


def run(*args, program_path=None, extra=None, expect=0):
    p = subprocess.run([program_path or program, "self-update", *args], env=dict(env, **(extra or {})), capture_output=True, text=True, timeout=240)
    if p.returncode != expect:
        fail("`self-update %s` exited %d, wanted %d\n%s\n%s" % (" ".join(args), p.returncode, expect, p.stdout, p.stderr))
    return p


def check():
    return json.loads(run("--check", "--json").stdout)


def snapshot():
    """Everything of the install: a refused update must leave it exactly as it was."""
    seen = {}
    for folder, _, names in os.walk(install):
        for name in names:
            with open(os.path.join(folder, name), "rb") as f:
                seen[os.path.relpath(os.path.join(folder, name), install)] = hashlib.sha256(f.read()).hexdigest()
    if os.name != "nt" and sys.platform != "darwin":
        seen["->current"] = os.readlink(os.path.join(install, "current"))
    return seen


def refused(label, wanted_note):
    before = snapshot()
    got = check()
    if got["available"] or got["canInstall"]:
        fail("%s: it was offered: %s" % (label, got))
    if wanted_note not in got["note"]:
        fail("%s: wanted a note about %r, got %r" % (label, wanted_note, got["note"]))
    run()  # and nothing happens when asked to update anyway
    if snapshot() != before:
        fail("%s: the install changed" % label)
    print("ok  refused: " + label)


try:
    asset = make_asset(NEW)
    env["SKILLHUB_UPDATE_REMOTE"] = tags_repo("tags", ["v0.9.0", "v1.0.0-beta1", "v" + NEW, "v3.0.0-dev", "something-else"])

    publish(asset)
    got = check()
    if not got["available"] or not got["canInstall"] or got["latest"] != NEW or got["current"] != "1.0.0-beta1":
        fail("a good release was not offered: %s" % got)
    wanted_kind = "setup" if os.name == "nt" else "app" if sys.platform == "darwin" else "portable"
    if got["kind"] != wanted_kind:
        fail("this install was taken for %s, not %s" % (got["kind"], wanted_kind))
    print("ok  a signed release is offered, as a %s install" % got["kind"])

    publish(asset, signature="changed")
    refused("a signature that was tampered with", "signature")
    publish(asset, signature="other key")
    refused("a signature by another key", "signature")
    publish(asset, signature=None)
    refused("a release without a signature", "SHA256SUMS.sig")
    publish(asset, sums="%s  some-other-file.exe\n" % ("0" * 64))
    refused("a signed list that does not name this system's file", "has no")
    shutil.rmtree(site, ignore_errors=True)
    os.makedirs(site)
    refused("a tag whose release is not out yet", "was passed over")

    # the list is good, the file is not: only downloading it can tell
    publish(asset, content=b"not what was signed")
    before = snapshot()
    if not check()["canInstall"]:
        fail("the list is good, so it should be offered")
    p = run(expect=1)
    if "does not match" not in p.stderr:
        fail("a changed file: %s" % p.stderr)
    if snapshot() != before:
        fail("a changed file reached the install")
    print("ok  refused: a file that does not match its signed checksum")

    if os.name != "nt":
        publish(make_asset("6.6.6"))
        before = snapshot()
        p = run(expect=1)
        if "does not start" not in p.stderr:
            fail("a build that is not the signed version: %s" % p.stderr)
        if snapshot() != before:
            fail("a build that is not the signed version was put in place")
        print("ok  refused: a signed build that does not start as that version")
        asset = make_asset(NEW)

    saved = env["SKILLHUB_UPDATE_REMOTE"]
    env["SKILLHUB_UPDATE_REMOTE"] = tags_repo("older", ["v0.9.0", "v1.0.0-beta1"])
    publish(asset, version="0.9.0")
    got = check()
    if got["available"] or got["latest"]:
        fail("it offered a downgrade: %s" % got)
    print("ok  never a downgrade")
    env["SKILLHUB_UPDATE_REMOTE"] = "http://127.0.0.1:1/nothing.git"
    p = run("--check", expect=1)
    if "could not reach" not in p.stderr:
        fail("an unreachable repository: %s" % p.stderr)
    print("ok  an unreachable repository is an error, not silence")
    env["SKILLHUB_UPDATE_REMOTE"] = saved

    # a copy that is not one of the installs that replace themselves: it says what to do, and touches nothing
    plain = os.path.join(top, "plain", os.path.basename(program))
    os.makedirs(os.path.dirname(plain))
    shutil.copyfile(BIN, plain)
    os.chmod(plain, os.stat(plain).st_mode | stat.S_IEXEC)
    publish(asset)
    got = json.loads(run("--check", "--json", program_path=plain).stdout)
    if not got["available"] or got["canInstall"] or not got["advice"]:
        fail("a copy that cannot replace itself: %s" % got)
    p = run(program_path=plain, expect=1)
    if got["advice"] not in p.stdout:
        fail("no advice: %s" % p.stdout)
    print("ok  a %s install gets a notice and nothing else" % got["kind"])

    # at last, the update itself
    publish(asset)
    p = run()
    if os.name == "nt":
        if "installer is running" not in p.stdout:
            fail("the installer was not started: %s\n%s" % (p.stdout, p.stderr))
    elif sys.platform == "darwin":
        said = subprocess.run([program, "--version"], capture_output=True, text=True).stdout.strip()
        if said != "skillhub " + NEW:
            fail("the app was not replaced: %s" % said)
        strays = [n for n in os.listdir(install) if ".new-" in n]
        if strays:
            fail("left behind: %s" % strays)
        if not [n for n in os.listdir(install) if ".old-" in n]:
            fail("the old app should stay until the next start")
    else:
        current = os.path.join(install, "current")
        if os.readlink(current) != NEW:
            fail("current was not switched: %s" % os.readlink(current))
        said = subprocess.run([os.path.join(current, "bin", "skillhub"), "--version"], capture_output=True, text=True).stdout.strip()
        if said != "skillhub " + NEW:
            fail("the new version does not answer: %s" % said)
        if not os.path.isfile(program):
            fail("the version that ran the update must stay until the next one")
        if [n for n in os.listdir(install) if n.startswith(".")]:
            fail("left behind: %s" % os.listdir(install))
        if shutil.which("wget") and shutil.which("tar"):
            # the same with wget, on a machine without curl
            os.remove(current)
            os.symlink("1.0.0-beta1", current)
            shutil.rmtree(os.path.join(install, NEW))
            tools = os.path.join(top, "tools")
            os.makedirs(tools)
            for tool in ("git", "tar", "wget", "gzip", "sh"):
                found = shutil.which(tool)
                if found:
                    os.symlink(found, os.path.join(tools, tool))
            run(extra={"PATH": tools})
            if os.readlink(current) != NEW:
                fail("wget did not get the update through")
            print("ok  wget does the same as curl")
    print("ok  the update itself")

    if WINDOW and sys.platform != "darwin":
        # the button in the window: the job runs, the backend stops, what the job named starts, and the window quits
        folder = os.path.dirname(program)
        shutil.copyfile(WINDOW, os.path.join(folder, os.path.basename(WINDOW)))
        os.chmod(os.path.join(folder, os.path.basename(WINDOW)), 0o755)
        if os.name == "nt":
            shutil.copyfile(BIN, os.path.join(folder, "skillhubw.exe"))  # what the window starts as its backend
        else:
            os.remove(os.path.join(install, "current"))
            os.symlink("1.0.0-beta1", os.path.join(install, "current"))
            shutil.rmtree(os.path.join(install, NEW))
        shot = os.path.join(top, "window.png")
        p = subprocess.run([os.path.join(folder, os.path.basename(WINDOW)), "gui", "--shot", shot, "--size", "800x600", "--action", "app-update", "--settle", "200"],
                           env=dict(env, QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software", SKILLHUB_REDUCE_MOTION="1"), capture_output=True, text=True, timeout=120)
        if p.returncode != 0:
            fail("the window ended with %d\n%s" % (p.returncode, p.stderr[-800:]))
        if os.path.exists(shot):
            fail("the update failed, so the window lived on to take its screenshot\n%s" % p.stderr[-800:])
        if os.name != "nt":
            if os.readlink(os.path.join(install, "current")) != NEW:
                fail("the button did not switch current")
            for _ in range(100):
                if os.path.exists(os.path.join(top, "new-window-started")):
                    break
                time.sleep(0.1)
            else:
                fail("the window quit, but the new version was never started")
        print("ok  the button in the window")
    print("self-update test passed")
finally:
    server.shutdown()
    shutil.rmtree(top, ignore_errors=True)
