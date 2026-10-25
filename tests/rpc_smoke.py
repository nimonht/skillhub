#!/usr/bin/env python3
"""The backend protocol (docs/protocol.md), offline, on every platform: `skillhub serve --stdio` driven the way the
window drives it.

usage: python3 tests/rpc_smoke.py PATH/TO/skillhub[-cli.exe|w.exe]

Covers the handshake and a version mismatch, malformed lines, a paged snapshot, a job with progress, log lines and a
delta, a change made by hand that the file watchers report, a cancelled job, the busy and locked errors (the command
line holding the mutation lock), settings, and shutting down in the middle of a job. Everything runs against a throw-away
home; the upstream repository is a local one the cache already holds.
"""
import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time

BIN = os.path.abspath(sys.argv[1])
WIN = os.name == "nt"
TIMEOUT = 60


def fail(msg):
    print("FAIL:", msg, file=sys.stderr)
    sys.exit(1)


def make_home():
    home = os.path.realpath(tempfile.mkdtemp(prefix="skillhub-rpc-"))
    env = dict(os.environ, HOME=home, USERPROFILE=home, SKILLHUB_HOME=home, XDG_CONFIG_HOME=home + "/.config",
               XDG_CACHE_HOME=home + "/.cache", XDG_STATE_HOME=home + "/.local/state", XDG_DATA_HOME=home + "/.local/share",
               SKILLHUB_BIN_PATH=os.path.join(home, "none"), SKILLHUB_CACHE=home + "/cache/skillhub",
               SKILLHUB_CONFIG=home + "/config/skillhub", SKILLHUB_STATE=home + "/state/skillhub")
    os.makedirs(os.path.join(home, ".claude"))  # Claude Code "installed": every skill gets a link
    return home, env


def skill(path, name, body="body"):
    os.makedirs(path, exist_ok=True)
    with open(os.path.join(path, "SKILL.md"), "w", newline="\n") as f:
        f.write("---\nname: %s\ndescription: test skill %s\n---\n\n%s\n" % (name, name, body))


def git(cwd, *args):
    p = subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.com", "-c", "core.autocrlf=false",
                        "-c", "init.defaultBranch=main", *args], cwd=cwd, capture_output=True, text=True, timeout=60)
    if p.returncode != 0:
        fail("git %s: %s" % (" ".join(args), p.stderr))
    return p.stdout.strip()


def upstream_repo(home, env, slug, names):
    """A local repository with the given skills, already cloned into the cache under the key of `slug`."""
    path = os.path.join(home, "upstream-" + slug.replace("/", "-"))
    for n in names:
        skill(os.path.join(path, "skills", n), n)
    git(path, "init", "-q")
    git(path, "add", "-A")
    git(path, "commit", "-q", "-m", "first")
    cached = os.path.join(env["SKILLHUB_CACHE"], "repos", slug.replace("/", "__"))
    os.makedirs(os.path.dirname(cached), exist_ok=True)
    git(home, "clone", "-q", "--depth", "1", "file://" + path.replace("\\", "/") if WIN else path, cached)
    return path


class Backend:
    def __init__(self, env):
        self.p = subprocess.Popen([BIN, "serve", "--stdio"], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE)
        self.messages = queue.Queue()
        self.seen = []
        self.next_id = 0
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=self._drain_stderr, daemon=True).start()

    def _read(self):
        for raw in self.p.stdout:
            try:
                self.messages.put(json.loads(raw))
            except ValueError:
                self.messages.put({"unparsable": raw})
        self.messages.put(None)

    def _drain_stderr(self):
        for _ in self.p.stderr:
            pass

    def send_raw(self, line):
        self.p.stdin.write(line if isinstance(line, bytes) else line.encode())
        self.p.stdin.flush()

    def request(self, method, params=None):
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            msg["params"] = params
        self.send_raw(json.dumps(msg) + "\n")
        return self.next_id

    def wait(self, match, what, timeout=TIMEOUT):
        """The first message (seen before, or arriving within `timeout`) that `match` accepts."""
        for m in self.seen:
            if match(m):
                self.seen.remove(m)
                return m
        end = time.time() + timeout
        while time.time() < end:
            try:
                m = self.messages.get(timeout=max(0.05, end - time.time()))
            except queue.Empty:
                break
            if m is None:
                fail("the backend ended while waiting for %s" % what)
            if "unparsable" in m:
                fail("the backend wrote a line that is not JSON: %r" % m["unparsable"])
            if match(m):
                return m
            self.seen.append(m)
        fail("timed out waiting for %s; seen: %s" % (what, json.dumps(self.seen)[:2000]))

    def response(self, rid, timeout=TIMEOUT):
        return self.wait(lambda m: m.get("id") == rid and "method" not in m, "the response to request %s" % rid, timeout)

    def call(self, method, params=None):
        r = self.response(self.request(method, params))
        if "error" in r:
            fail("%s failed: %s" % (method, r["error"]))
        return r["result"]

    def error_of(self, method, params=None):
        r = self.response(self.request(method, params))
        if "error" not in r:
            fail("%s should have failed: %s" % (method, r))
        return r["error"]["code"]

    def note(self, method, match=lambda p: True, timeout=TIMEOUT, why=""):
        what = "%s (%s)" % (method, why) if why else method
        return self.wait(lambda m: m.get("method") == method and match(m.get("params", {})), what, timeout)["params"]

    def job_done(self, job):
        return self.note("event.jobDone", lambda p: p.get("job") == job)

    def close(self):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        return self.p.wait(timeout=TIMEOUT)


def snapshot(b, page_size=1000):
    """Every page at one revision, as the window loads it."""
    first = b.call("state.snapshot", {"page": 0, "pageSize": page_size})
    skills = list(first["skills"])
    for page in range(1, first["pages"]):
        p = b.call("state.snapshot", {"page": page, "pageSize": page_size})
        if p["rev"] != first["rev"]:
            return snapshot(b, page_size)  # a delta came in between: start over
        skills += p["skills"]
    return first, {s["name"]: s for s in skills}


def lock_file(path):
    """Take the mutation lock the way another Skill Hub process would."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    f = open(path, "a+b")
    if WIN:
        import msvcrt
        f.seek(0)
        msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
    else:
        import fcntl
        fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    return f


def main():
    home, env = make_home()
    try:
        central = os.path.join(home, ".agents", "skills")
        for n in ("alpha", "beta", "gamma"):
            skill(os.path.join(central, n), n)
        upstream = upstream_repo(home, env, "acme/skills", ["one", "two"])

        b = Backend(env)
        # nothing before the handshake, and a window of another protocol version is turned away
        if b.error_of("state.snapshot") != -32600:
            fail("a request before initialize was accepted")
        if b.error_of("initialize", {"protocol": 99}) != -32002:
            fail("a different protocol version was accepted")
        init = b.call("initialize", {"protocol": 1, "client": "rpc_smoke"})
        if init["protocol"] != 1 or not init["version"] or init["paths"]["central"].replace("\\", "/") != central.replace("\\", "/"):
            fail("initialize result: %s" % init)

        # malformed lines get errors, never silence or a crash
        b.send_raw("this is not json\n")
        if b.wait(lambda m: m.get("id") is None and "error" in m, "a parse error")["error"]["code"] != -32700:
            fail("not JSON: wrong error")
        b.send_raw('{"jsonrpc":"2.0","id":"x"}\n')
        if b.response("x")["error"]["code"] != -32600:
            fail("a request without a method: wrong error")
        b.send_raw('[{"jsonrpc":"2.0","id":1,"method":"settings.get"}]\n')
        if b.wait(lambda m: m.get("id") is None and "error" in m, "a batch error")["error"]["code"] != -32600:
            fail("a batch was not refused")
        if b.error_of("no.such.method") != -32601:
            fail("an unknown method: wrong error")
        b.send_raw("\n   \n")  # blank lines are ignored

        # a paged snapshot
        first, rows = snapshot(b, page_size=2)
        if first["pages"] != 2 or set(rows) != {"alpha", "beta", "gamma"}:
            fail("paged snapshot: %s pages, %s" % (first["pages"], sorted(rows)))
        if [a["id"] for a in first["agents"]] != ["claude-code"] or first["counts"]["skills"] != 3:
            fail("snapshot agents or counts: %s %s" % (first["agents"], first["counts"]))
        if rows["alpha"]["status"] != "local" or rows["alpha"]["links"] != ["missing"] or not rows["alpha"]["problem"]:
            fail("a local skill without its link: %s" % rows["alpha"])
        rev = first["rev"]
        text = b.call("skills.read", {"name": "alpha"})["text"]
        if "name: alpha" not in text:
            fail("skills.read: %r" % text)
        if b.error_of("skills.read", {"name": "missing-one"}) != 1004:
            fail("skills.read of a missing skill: wrong error")

        # a job: install from GitHub (the cached local clone), with progress, log lines and a delta
        job = b.call("skills.install", {"source": "acme/skills", "skills": ["one", "two"]})["job"]
        done = b.job_done(job)
        if not done["ok"] or done["summary"]["installed"] != 2:
            fail("install job: %s" % done)
        progress = [m for m in b.seen if m.get("method") == "event.progress" and m["params"]["job"] == job]
        if not progress or progress[-1]["params"]["done"] != progress[-1]["params"]["total"]:
            fail("install job progress: %s" % progress)
        logs = [l["text"] for m in b.seen if m.get("method") == "event.log" for l in m["params"]["lines"]]
        if not any(t.startswith("installed one from acme/skills") for t in logs):
            fail("install job log lines: %s" % logs)
        delta = b.note("state.delta", lambda p: any(u["name"] == "one" for u in p["upserts"]), why="after install")
        if delta["baseRev"] != rev or delta["counts"]["skills"] != 5:
            fail("delta after install: %s" % delta)
        # the install's one relink at the end linked the skills that were there before too
        if not any(u["name"] == "alpha" and u["links"] == ["link"] for u in delta["upserts"]):
            fail("the skills already there were not linked by the install's relink: %s" % delta["upserts"])
        rev = delta["rev"]
        b.seen.clear()

        # adopt finds nothing left to do; a change made by hand afterwards reaches the window through the file watchers
        done = b.job_done(b.call("adopt")["job"])
        if done["summary"]["changes"] != 0:
            fail("adopt after install: %s" % done)
        time.sleep(1.0)  # past the quiet time after a job, so the watchers are listened to again
        skill(os.path.join(central, "by-hand"), "by-hand")
        delta = b.note("state.delta", lambda p: any(u["name"] == "by-hand" for u in p["upserts"]), why="a skill added by hand")
        shutil.rmtree(os.path.join(central, "by-hand"))
        b.note("state.delta", lambda p: "by-hand" in p["removes"], why="a skill removed by hand")

        # check: nothing changed upstream; then a change, and the status follows
        done = b.job_done(b.call("skills.check")["job"])
        if not done["ok"] or done["summary"]["available"] != 0:
            fail("check with nothing new: %s" % done)
        with open(os.path.join(upstream, "skills", "one", "SKILL.md"), "a", newline="\n") as f:
            f.write("\nversion 2\n")
        git(upstream, "commit", "-q", "-am", "second")
        done = b.job_done(b.call("skills.check")["job"])
        if done["summary"]["available"] != 1 or not any(r["name"] == "one" and r["status"] == "available" for r in done["results"]):
            fail("check after an upstream change: %s" % done)
        b.note("state.delta", lambda p: any(u["name"] == "one" and u["status"] == "available" for u in p["upserts"]), why="check after an upstream change")

        # one job at a time; the command line's lock keeps changing jobs out
        job = b.call("skills.check")["job"]
        if b.error_of("adopt") != 1001:
            fail("a second job was not refused as busy")
        b.job_done(job)
        held = lock_file(os.path.join(env["SKILLHUB_STATE"], "mutate.lock"))
        if b.error_of("adopt") != 1002:
            fail("a changing job ran while another process held the mutation lock")
        b.job_done(b.call("skills.check")["job"])  # reading upstream needs no mutation lock
        held.close()

        # update; the status becomes "updated"
        done = b.job_done(b.call("skills.update", {"names": ["one"]})["job"])
        if not done["ok"] or done["summary"]["updated"] != 1:
            fail("update job: %s" % done)
        b.note("state.delta", lambda p: any(u["name"] == "one" and u["status"] == "updated" for u in p["upserts"]), why="after update")
        if "version 2" not in open(os.path.join(central, "one", "SKILL.md"), encoding="utf-8").read():
            fail("update did not install the new version")

        # a job can be cancelled: it stops at a safe point and says so (or had finished already)
        job = b.call("skills.check")["job"]
        r = b.response(b.request("jobs.cancel", {"job": job}))
        if "error" in r and r["error"]["code"] != 1004:
            fail("jobs.cancel: %s" % r)
        done = b.job_done(job)
        if "error" not in r and not done.get("cancelled") and not done["ok"]:
            fail("a cancelled job: %s" % done)

        # settings: merged, announced, and kept on disk
        settings = b.call("settings.set", {"patch": {"theme": "dark", "accent": "#7c5cff"}})
        if settings.get("theme") != "dark":
            fail("settings.set: %s" % settings)
        b.note("settings.changed", lambda p: p["settings"].get("accent") == "#7c5cff")
        settings = b.call("settings.set", {"patch": {"accent": None}})
        if "accent" in settings or b.call("settings.get").get("theme") != "dark":
            fail("settings.set with null should remove the key: %s" % settings)

        # agents
        providers = b.call("providers.list")["providers"]
        if not any(p["id"] == "claude-code" and p["managed"] for p in providers):
            fail("providers.list: %s" % providers[:3])
        status = b.call("automation.status")
        if "timer" not in status or "supported" not in status:
            fail("automation.status: %s" % status)

        # shutting down in the middle of a job: it stops at a safe point, then the answer comes
        job = b.call("skills.check")["job"]
        sd = b.call("shutdown")
        if sd["cancelled"] not in ([job], []):
            fail("shutdown result: %s" % sd)
        if b.error_of("settings.get") != 1003:
            fail("a request after shutdown was accepted")
        code = b.close()
        if code != 0:
            fail("the backend exited with %d" % code)

        # a backend whose window went away (standard input closed) exits by itself
        b2 = Backend(env)
        b2.call("initialize", {"protocol": 1})
        if b2.close() != 0:
            fail("the backend did not exit cleanly when its input closed")
        print("rpc smoke test passed (%s)" % sys.platform)
    finally:
        shutil.rmtree(home, ignore_errors=True)


main()
