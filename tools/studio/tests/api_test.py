#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""api_test.py — Kiln Studio's server, attacked and exercised, with each defence proven.

    python3 tools/studio/tests/api_test.py

Runs the server against a fake `nix` (so no build happens) and a fixture
manifest, and checks:

  * no credentials → 401; a wrong session token → 401; the right one → a cookie
  * a Host header naming another site → 403 (DNS rebinding)
  * a POST from a foreign Origin, or with none → 403 (CSRF)
  * Tailscale identity headers are believed from the proxy address, ignored from
    anywhere else, and refused for a login not on the list
  * a job for a target the manifest does not name, or one carrying shell syntax,
    → 400 with nothing started
  * a build job streams its log over SSE in order, ends ok, and reports its output
    path; a reconnect with Last-Event-ID resumes exactly after that line
  * cancelling a job kills the whole process tree, not only `nix`
  * static paths cannot climb out of the studio directory
  * every response carries the security headers

Then it proves the tests are what they claim: it restarts the server once per
guard with KILN_STUDIO_BREAK=<guard> and requires at least one failure each time.
A test suite that still passes with the host check switched off is not testing
the host check.
"""

import http.client
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
SERVER = HERE.parent / "server.py"
GUARDS = ("host", "origin", "token", "identity-source", "argv", "traversal", "cancel-group")
ALICE = "alice@example.com"

FAKE_NIX = r'''#!@PYTHON@
import json, os, subprocess, sys, time
args = sys.argv[1:]
ref = args[-1] if args else ""
attr = ref.split("#", 1)[1] if "#" in ref else ""
def say(msg):
    sys.stderr.write("@nix " + json.dumps({"action": "msg", "level": 0, "msg": msg}) + "\n")
    sys.stderr.flush()
if args[:1] == ["build"]:
    if attr.endswith("slow-demo"):
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(120)"])
        with open(os.environ["FAKE_NIX_PIDFILE"], "w") as f:
            f.write(str(child.pid))
        for i in range(1200):
            say(f"slow {i}")
            time.sleep(0.1)
    for i in range(1, 21):
        say(f"line {i}")
    sys.stderr.write("@nix " + json.dumps({"action": "result", "type": 101, "fields": ["a build log line"]}) + "\n")
    print("/nix/store/0000-fake-" + attr.replace(".", "-"))
    sys.exit(3 if attr.endswith("broken-demo") else 0)
sys.exit(1)
'''

MANIFEST = {
    "version": 1, "system": "x86_64-linux", "cheap": ["fast-check"],
    "games": {"demo": {"roms": ["demo"], "romTitle": "Demo", "jumps": [], "pc": [], "web": [], "hostable": False}},
    "packages": {n: {"kind": "rom", "example": "demo"} for n in ("demo", "slow-demo", "broken-demo")},
    "tools": [], "checks": ["fast-check"],
}


class Server:
    def __init__(self, work, breaks=()):
        self.work = work
        env = dict(os.environ, KILN_STUDIO_BREAK=",".join(breaks), FAKE_NIX_PIDFILE=str(work / "child.pid"))
        self.proc = subprocess.Popen(
            [sys.executable, str(SERVER), "--repo", str(work / "repo"), "--port", "0",
             "--manifest-file", str(work / "manifest.json"), "--caps-file", str(work / "caps.json"),
             "--nix", str(work / "nix"), "--tailscale-login", ALICE, "--trusted-proxy", "127.0.0.1"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, env=env)
        line = self.proc.stdout.readline().split()
        assert line[:1] == ["listening"], f"server did not start: {line}"
        self.port = int(line[2])
        self.token = (work / "repo" / ".studio" / "token").read_text().strip()

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def request(self, method, path, body=None, headers=None, source="127.0.0.1", auth=True, timeout=10):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout, source_address=(source, 0))
        h = {"Host": f"127.0.0.1:{self.port}"}
        if method == "POST":
            h["Origin"] = f"http://127.0.0.1:{self.port}"
            h["Content-Type"] = "application/json"
        if auth:
            h["Authorization"] = f"Bearer {self.token}"
        h.update(headers or {})
        h = {k: v for k, v in h.items() if v is not None}
        conn.request(method, path, body=json.dumps(body) if body is not None else None, headers=h)
        r = conn.getresponse()
        data = r.read()
        conn.close()
        return r.status, dict(r.getheaders()), data


def sse(server, jid, last=None, timeout=20):
    conn = http.client.HTTPConnection("127.0.0.1", server.port, timeout=timeout)
    h = {"Host": f"127.0.0.1:{server.port}", "Authorization": f"Bearer {server.token}"}
    if last is not None:
        h["Last-Event-ID"] = str(last)
    conn.request("GET", f"/api/jobs/{jid}/events", headers=h)
    r = conn.getresponse()
    events, cur = [], {}
    while True:
        line = r.fp.readline()
        if not line:
            break
        line = line.decode().rstrip("\n")
        if line == "":
            if cur:
                events.append(cur)
                if cur.get("event") == "done":
                    break
            cur = {}
            continue
        if line.startswith(":"):
            continue
        k, _, v = line.partition(": ")
        cur[k] = v
    conn.close()
    return events


def alive(pid):
    try:
        state = Path(f"/proc/{pid}/status").read_text()
    except OSError:
        return False
    return "\nState:\tZ" not in state


def suite(server):
    fails = []

    def expect(cond, msg):
        if not cond:
            fails.append(msg)

    # ── identity ─────────────────────────────────────────────────────────
    st, _, _ = server.request("GET", "/api/manifest", auth=False)
    expect(st == 401, f"no credentials gave {st}, want 401")
    st, _, _ = server.request("GET", "/api/manifest", auth=False, headers={"Authorization": "Bearer wrong"})
    expect(st == 401, f"a wrong bearer token gave {st}, want 401")
    st, _, _ = server.request("POST", "/api/session", body={"token": "wrong"}, auth=False)
    expect(st == 401, f"a wrong session token gave {st}, want 401")
    st, hdrs, _ = server.request("POST", "/api/session", body={"token": server.token}, auth=False)
    cookie = hdrs.get("Set-Cookie", "")
    expect(st == 204 and "HttpOnly" in cookie and "SameSite=Strict" in cookie,
           f"the right session token gave {st} with cookie {cookie!r}")
    st, _, body = server.request("GET", "/api/whoami", auth=False, headers={"Cookie": cookie.split(";")[0]})
    expect(st == 200 and json.loads(body or b"{}").get("via") == "token", f"the session cookie gave {st}")

    # ── tailscale identity ───────────────────────────────────────────────
    st, _, body = server.request("GET", "/api/whoami", auth=False, headers={"Tailscale-User-Login": ALICE})
    expect(st == 200 and json.loads(body or b"{}").get("user") == ALICE,
           f"an allowlisted login from the proxy gave {st} {body[:80]!r}")
    st, _, _ = server.request("GET", "/api/whoami", auth=False, headers={"Tailscale-User-Login": "mallory@evil"})
    expect(st == 401, f"a login not on the list gave {st}, want 401")
    st, _, _ = server.request("GET", "/api/whoami", auth=False, source="127.0.0.2",
                              headers={"Tailscale-User-Login": ALICE})
    expect(st == 401, f"identity headers from a non-proxy address gave {st}, want 401")

    # ── host and origin ──────────────────────────────────────────────────
    st, _, _ = server.request("GET", "/api/whoami", headers={"Host": "attacker.example:80"})
    expect(st == 403, f"a foreign Host gave {st}, want 403")
    st, _, _ = server.request("POST", "/api/jobs", body={"kind": "build", "target": "demo"},
                              headers={"Origin": "http://attacker.example"})
    expect(st == 403, f"a foreign Origin POST gave {st}, want 403")
    st, _, _ = server.request("POST", "/api/jobs", body={"kind": "build", "target": "demo"}, headers={"Origin": None})
    expect(st == 403, f"a POST with no Origin gave {st}, want 403")

    # ── argv ─────────────────────────────────────────────────────────────
    for target in ("not-in-manifest", "demo; rm -rf /", "../../etc", "-rf"):
        st, _, _ = server.request("POST", "/api/jobs", body={"kind": "build", "target": target})
        expect(st == 400, f"build target {target!r} gave {st}, want 400")
    st, _, _ = server.request("POST", "/api/jobs", body={"kind": "shell", "target": "demo"})
    expect(st == 400, f"job type 'shell' gave {st}, want 400")
    st, _, _ = server.request("POST", "/api/jobs", body={"kind": "validate", "target": "rm"})
    expect(st == 400, f"validator 'rm' gave {st}, want 400")
    for arg in ("../../etc/passwd", "assets/not-there.map", "/etc/passwd"):
        st, _, _ = server.request("POST", "/api/jobs", body={"kind": "validate", "target": "map-validate", "arg": arg})
        expect(st == 400, f"map-validate of {arg!r} gave {st}, want 400")
    st, _, body = server.request("GET", "/api/validators")
    listed = {v["id"]: v["args"] for v in json.loads(body or b"{}").get("validators", [])}
    expect(st == 200 and listed.get("map-validate") == ["assets/level.map"],
           f"validators list gave {st} {listed.get('map-validate')}")
    st, _, _ = server.request("POST", "/api/jobs", body={"kind": "validate", "target": "map-validate",
                                                          "arg": "assets/level.map"})
    expect(st == 201, f"map-validate of a listed map gave {st}, want 201")

    # ── a build, streamed ────────────────────────────────────────────────
    st, _, body = server.request("POST", "/api/jobs", body={"kind": "build", "target": "demo"})
    job = json.loads(body or b"{}")
    expect(st == 201 and job.get("id"), f"a valid build gave {st}")
    if job.get("id"):
        events = sse(server, job["id"])
        lines = [e for e in events if e.get("event") == "line"]
        ids = [int(e["id"]) for e in lines]
        expect(ids == list(range(1, len(ids) + 1)), f"SSE ids are not 1..n in order: {ids[:8]}…")
        texts = [json.loads(e["data"]) for e in lines]
        numbered = [t for t in texts if t.startswith("line ")]
        expect(numbered == [f"line {i}" for i in range(1, 21)], "the build's log lines arrived out of order or incomplete")
        done = [e for e in events if e.get("event") == "done"]
        snap = json.loads(done[0]["data"]) if done else {}
        expect(snap.get("state") == "ok" and any("fake-demo" in o for o in snap.get("outputs", [])),
               f"the build ended {snap.get('state')} with outputs {snap.get('outputs')}")
        resumed = [e for e in sse(server, job["id"], last=10) if e.get("event") == "line"]
        expect(resumed and resumed[0]["id"] == "11", f"resume after id 10 started at {resumed[0]['id'] if resumed else None}")

    # ── cancel kills the tree ────────────────────────────────────────────
    pidfile = server.work / "child.pid"
    pidfile.unlink(missing_ok=True)
    st, _, body = server.request("POST", "/api/jobs", body={"kind": "build", "target": "slow-demo"})
    slow = json.loads(body or b"{}")
    if slow.get("id"):
        deadline = time.time() + 10
        while not pidfile.exists() and time.time() < deadline:
            time.sleep(0.1)
        child = int(pidfile.read_text()) if pidfile.exists() else None
        st, _, _ = server.request("POST", f"/api/jobs/{slow['id']}/cancel", body={})
        expect(st == 200, f"cancel gave {st}")
        deadline = time.time() + 8
        while child and alive(child) and time.time() < deadline:
            time.sleep(0.1)
        expect(child is not None and not alive(child), "cancel left the build's child process running")
        if child and alive(child):
            os.kill(child, 9)

    # ── static files stay inside the studio ──────────────────────────────
    for path in ("/src/../server.py", "/src/%2e%2e/%2e%2e/%2e%2e/etc/passwd", "/../../etc/passwd", "/server.py"):
        st, _, body = server.request("GET", path, auth=False)
        expect(st == 404, f"static {path} gave {st}, want 404")
    st, hdrs, body = server.request("GET", "/", auth=False)
    expect(st == 200 and b"Kiln Studio" in body, f"the page itself gave {st}")
    expect("default-src 'self'" in hdrs.get("Content-Security-Policy", "") and hdrs.get("X-Content-Type-Options") == "nosniff",
           "security headers missing")
    return fails


def setup(work):
    (work / "repo").mkdir()
    (work / "repo" / "assets").mkdir()
    (work / "repo" / "assets" / "level.map").write_text("{\n\"classname\" \"worldspawn\"\n}\n")
    (work / "manifest.json").write_text(json.dumps(MANIFEST))
    (work / "caps.json").write_text(json.dumps({"system": "x86_64-linux", "ares": False}))
    nix = work / "nix"
    nix.write_text(FAKE_NIX.replace("@PYTHON@", sys.executable))
    nix.chmod(0o755)


def main():
    total = 0
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        print("── the suite, every guard on ──")
        work = base / "clean"
        work.mkdir()
        setup(work)
        s = Server(work)
        try:
            fails = suite(s)
        finally:
            s.stop()
        for f in fails:
            print(f"  FAIL {f}")
        print(f"  {'ok' if not fails else 'FAILED'} ({len(fails)} failure(s))")
        total += len(fails)

        print("── each guard switched off must fail the suite ──")
        for guard in GUARDS:
            work = base / f"break-{guard}"
            work.mkdir()
            setup(work)
            s = Server(work, breaks=(guard,))
            try:
                fails = suite(s)
            finally:
                s.stop()
            ok = bool(fails)
            print(f"  {'ok  ' if ok else 'FAIL'} without {guard}: {len(fails)} failure(s)"
                  + (f" — e.g. {fails[0]}" if fails else " — THE SUITE DID NOT NOTICE"))
            total += 0 if ok else 1

    print("studio-api: " + ("FAILED" if total else "ok"))
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
