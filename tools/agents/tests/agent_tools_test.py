#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""agent_tools_test.py — the agent tools, offline, no model.

Proves the licence each tool claims:

  * every fs tool refuses a path outside the worktree (../.., absolute, a
    symlink pointing out);
  * WriteFile/EditFile round-trip inside it;
  * Git answers status and refuses push/checkout/branch surgery;
  * StudioJob posts to a STUDIO (a stub here) with kind+target as JSON and
    reads a settled job's report — and refuses an unknown kind;
  * ClaudeWorker builds exactly the argv the plan pins (a fake `claude` on
    PATH records it) and reports the worker's final message + diff stat.

Run by nix/checks/agent-tools.nix; exits non-zero on any failure.
"""

import json
import os
import stat
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # tools/agents on sys.path
os.environ.setdefault("CREWAI_DISABLE_TELEMETRY", "true")

from kiln_agents.tools import ClaudeWorker, EditFile, Git, Grep, ListDir, ReadFile, WriteFile
from kiln_agents.tools import role_tools
from kiln_agents.tools.claudeworker import ALLOWED_TOOLS


def main():
    wt = Path(tempfile.mkdtemp(prefix="kiln-agent-tools-"))
    (wt / "src").mkdir()
    (wt / "src" / "main.c").write_text("int main(void) { return 0; }\n")

    subprocess.run(["git", "init", "-q", "-b", "agent/task-1", str(wt)], check=True)
    subprocess.run(["git", "-C", str(wt), "add", "."], check=True)
    subprocess.run(["git", "-C", str(wt), "-c", "user.email=t@t", "-c", "user.name=t",
                    "commit", "-qm", "base"], check=True)

    # --- fs scoping -----------------------------------------------------------
    # tools raise ValueError (read/write/list) or return a refusal; both count
    for tool, args in [(ReadFile, {"path": "../evil"}), (ReadFile, {"path": "/etc/passwd"}),
                       (WriteFile, {"path": "../evil", "content": "x"}),
                       (EditFile, {"path": "/etc/passwd", "old": "root", "new": "x"}),
                       (ListDir, {"path": ".."}), (Grep, {"pattern": "x", "path": "../"})]:
        inst = tool(worktree=str(wt))
        try:
            out = inst._run(**args)
            assert "outside the task worktree" in str(out), out
        except ValueError as e:
            assert "outside the task worktree" in str(e), e
        print(f"  ok   {inst.name} refuses {args['path']!r}")

    # a symlink inside the worktree pointing OUT of it must still refuse
    link = wt / "escape"
    link.symlink_to("/tmp")
    try:
        out = ReadFile(worktree=str(wt))._run(path="escape/whatever")
        assert "outside the task worktree" in out, out
    except ValueError:
        pass
    print("  ok   a symlink out of the worktree is refused")

    w = WriteFile(worktree=str(wt))._run(path="src/new.c", content="// new\n")
    assert "wrote" in w
    e = EditFile(worktree=str(wt))._run(path="src/new.c", old="// new", new="// changed")
    assert "edited" in e
    e2 = EditFile(worktree=str(wt))._run(path="src/new.c", old="not there", new="x")
    assert "refused" in e2 and "0 times" in e2
    g = Grep(worktree=str(wt))._run(pattern="changed")
    assert "new.c" in g
    print("  ok   write/edit/grep round-trip inside the worktree")

    # --- git licence ----------------------------------------------------------
    git = Git(worktree=str(wt))
    assert "agent/task-1" in git._run(op="status")
    for bad in [{"op": "push"}, {"op": "checkout"}, {"op": "reset"}, {"op": "merge"}]:
        out = git._run(**bad)
        assert "refused" in out, out
        print(f"  ok   git refuses {bad['op']}")
    out = git._run(op="add", paths=["/etc/passwd"])
    assert "outside the task worktree" in out, out
    print("  ok   git add refuses an out-of-worktree path")

    # --- StudioJob against a stub studio --------------------------------------
    received = {}

    class Stub(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _j(self, obj, code=200):
            b = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b)

        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            received["body"] = json.loads(self.rfile.read(n) or b"{}")
            received["origin"] = self.headers.get("Origin")
            self._j({"id": "j9"}, 201)

        def do_GET(self):
            if self.path == "/api/jobs/j9":
                self._j({"id": "j9", "state": "ok", "exit": 0,
                         "report": {"ok": True, "errors": []}})
            else:
                self._j({"error": "not found"}, 404)

    srv = ThreadingHTTPServer(("127.0.0.1", 0), Stub)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    os.environ["KILN_STUDIO_URL"] = f"http://127.0.0.1:{srv.server_address[1]}"
    os.environ["KILN_STUDIO_TOKEN"] = "t"

    from kiln_agents.tools.studiojob import StudioJob
    sj = StudioJob()
    out = sj._run(kind="shell", target="x")  # not a real kind
    # a refusal is a tool answer the model can read, not a crash — and the
    # studio must never have been asked
    assert "refused" in out and not received, (out, received)
    print("  ok   StudioJob refuses an unknown kind without asking the studio")
    out = json.loads(sj._run(kind="validate", target="map-validate",
                             path="assets/map_demo.map"))
    assert out["state"] == "ok" and out["report"]["ok"] is True, out
    assert received["body"] == {"kind": "validate", "target": "map-validate",
                                "arg": "assets/map_demo.map"}, received
    assert received["origin"] == os.environ["KILN_STUDIO_URL"]
    print("  ok   StudioJob posts kind+target(+arg) with the studio's own Origin")

    # --- ClaudeWorker's argv are pinned ---------------------------------------
    bindir = wt / "bin"
    bindir.mkdir()
    (bindir / "claude").write_text(
        "#!/bin/sh\n"
        'echo "$@" > "' + str(wt) + '/argv.txt"\n'
        "cat >/dev/null\n"
        'echo \'{\\"type\\":\\"result\\",\\"result\\":\\"done the thing\\"}\'\n'
        "exit 0\n".replace('\\"', '"'))
    os.chmod(bindir / "claude", os.stat(bindir / "claude").st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    os.environ["PATH"] = f"{bindir}:{os.environ['PATH']}"

    cw = ClaudeWorker(worktree=str(wt), claude_bin=str(bindir / "claude"))
    pinned = cw._argv()
    assert pinned[1:] == ["-p", "--output-format", "stream-json",
                          "--permission-mode", "acceptEdits",
                          "--allowedTools", ALLOWED_TOOLS], pinned
    print("  ok   ClaudeWorker's argv are exactly the pinned ones")
    out = json.loads(cw._run(brief="do the thing", budget_s=60))
    assert out["ok"] is True and "done the thing" in out["final"], out
    argv_seen = (wt / "argv.txt").read_text()
    assert "-p" in argv_seen and "acceptEdits" in argv_seen
    print("  ok   the worker's final message and diff stat come back")

    # --- role toolsets: no role gets everything -------------------------------
    names = {role: sorted(t.name for t in role_tools(role, str(wt)))
             for role in ("planner", "engineer", "content", "validator", "reviewer")}
    assert "claude_worker" in names["engineer"] and "claude_worker" not in names["content"], names
    assert "write_file" not in names["reviewer"], names
    assert "write_file" not in names["validator"], names
    print("  ok   role toolsets: only the engineer gets claude_worker; reviewers and "
          "validators cannot write")
    srv.shutdown()
    print("agent-tools test: ok")


if __name__ == "__main__":
    main()
