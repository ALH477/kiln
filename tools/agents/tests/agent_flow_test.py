#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""agent_flow_test.py — KilnTaskFlow end to end, offline, against scripts.

No model is called: KILN_AGENTS_LLM_STUB points models.py at an in-process
stub speaking the OpenAI chat-completions API, answered from a SCRIPT — the
planner's reply, the worker's reply, the reviewer. Every chat call is also
recorded, so the check afterwards can assert the flow actually consulted the
planner and the reviewer (a flow that skipped steps would show too few calls).

And no studio either: the SAME stub process answers the studio API (job
submit → settled job with a report), scripted to go RED once and GREEN after
— driving the validate → work loop past MAX_ROUNDS 0 → 1, then through
review, await_human, and the persisted re-kick with a human decision.

Also asserted, per the plan: the telemetry env is set by importing the
package, and no OpenAI env var appeared on the way.

Run by nix/checks/agent-flow.nix; exits non-zero on any failure.
"""

import json
import os
import subprocess
import sys
import tempfile
import threading
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

# --- script -------------------------------------------------------------------
# Order matters: intake consults nothing; plan → one chat call; work → one
# (per round); review → one. The studio script flips red→green on the 2nd
# validate round, so the sequence is plan, work(1), work(2), review.

CHATS = [
    "Plan: edit README.md in the worktree.\nkind: docs\nchecks: studio-manifest",
    "Did the work and committed it. commit deadbeef",
    "Did the work again, properly this time. commit cafebabe",
    "APPROVE — the diff is the docs change the task asked for.",
]
CHAT_CALLS = []
STUDIO_VALIDATIONS = {"rounds": 0}


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
        body = json.loads(self.rfile.read(n) or b"{}")

        if self.path.endswith("/chat/completions"):
            CHAT_CALLS.append([m.get("role") for m in body.get("messages", [])])
            idx = min(len(CHAT_CALLS) - 1, len(CHATS) - 1)
            return self._j({"id": "chatcmpl-stub", "object": "chat.completion",
                            "choices": [{"index": 0, "finish_reason": "stop",
                                         "message": {"role": "assistant",
                                                     "content": CHATS[idx]}}],
                            "usage": {"prompt_tokens": 1, "completion_tokens": 1,
                                      "total_tokens": 2}})

        if self.path == "/api/jobs":
            # every submitted check: round 1 red, round 2 green
            STUDIO_VALIDATIONS["rounds"] += 1
            ok = STUDIO_VALIDATIONS["rounds"] >= 2
            return self._j({"id": f"job{STUDIO_VALIDATIONS['rounds']}",
                            "_ok": ok}, 201)

        self._j({"error": "not found"}, 404)

    def do_GET(self):
        if self.path.startswith("/api/jobs/") and self.path != "/api/jobs/events":
            rid = self.path.rsplit("/", 1)[1]
            n = int(rid[3:])
            ok = n >= 2
            return self._j({"id": rid, "state": "ok", "exit": 0 if ok else 1,
                            "report": {"tool": "stub", "ok": ok,
                                       "errors": [] if ok else [{"code": "E", "msg": "red"}]}})
        self._j({"error": "not found"}, 404)

    def do_PATCH(self):
        n = int(self.headers.get("Content-Length") or 0)
        self.rfile.read(n)
        self._j({"ok": True})


def main():
    import kiln_agents  # noqa: F401 — the import itself must set telemetry env
    for var, want in {"CREWAI_DISABLE_TELEMETRY": "true", "OTEL_SDK_DISABLED": "true",
                      "CREWAI_TRACING_ENABLED": "false"}.items():
        assert os.environ.get(var) == want, f"{var}={os.environ.get(var)!r} — import must set {want}"
    print("  ok   importing the package sets the telemetry-off env")

    assert "OPENAI_API_KEY" not in os.environ, "an OpenAI key must never appear"
    print("  ok   no OPENAI_API_KEY in the environment")

    srv = ThreadingHTTPServer(("127.0.0.1", 0), Stub)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{srv.server_address[1]}"
    os.environ["KILN_AGENTS_LLM_STUB"] = base + "/v1"
    os.environ["KILN_STUDIO_URL"] = base
    os.environ["KILN_STUDIO_TOKEN"] = "t"

    # a worktree-shaped directory (a real git repo — the flow walks it)
    wt = Path(tempfile.mkdtemp(prefix="kiln-agent-flow-"))
    (wt / "README.md").write_text("# task worktree\n")
    subprocess.run(["git", "init", "-q", "-b", "agent/t1", str(wt)], check=True)
    subprocess.run(["git", "-C", str(wt), "add", "."], check=True)
    subprocess.run(["git", "-C", str(wt), "-c", "user.email=t@t", "-c", "user.name=t",
                    "commit", "-qm", "base"], check=True)

    os.environ["KILN_AGENTS_DB"] = str(wt / "flows.db")
    # FLOW_PERSISTENCE and not a fresh instance: crewai's @persist saves
    # through the module-level object baked in at class decoration; anything
    # else here loads from a db nothing was saved to (flow.py explains).
    # KILN_AGENTS_DB must be set before the module import picks it up.
    from kiln_agents.flow import FLOW_PERSISTENCE, KilnTaskFlow

    flow = KilnTaskFlow(persistence=FLOW_PERSISTENCE)
    flow.kickoff(inputs={"id": "task-flow-test", "task_id": "task-flow-test",
                         "brief": "update the README", "worktree": str(wt)})

    s = flow.state
    assert s.kind == "docs", s
    assert s.checks == ["studio-manifest"], s.checks
    assert s.rounds == 2 and s.validation_ok is True, s.validations
    assert "APPROVE" in s.review, s.review
    assert s.approved is None, "no human yet — approve must not happen by itself"
    print("  ok   plan → work(red) → work(green) → review → await_human, "
          f"{len(CHAT_CALLS)} model calls, {STUDIO_VALIDATIONS['rounds']} validate rounds")

    # the human approves in the studio; serve.py would re-kick the same id
    flow2 = KilnTaskFlow(persistence=FLOW_PERSISTENCE)
    flow2.kickoff(inputs={"id": "task-flow-test", "approved": True})
    s2 = flow2.state
    assert s2.approved is True and s2.report.get("approved") is True, s2
    assert s2.plan == s.plan and s2.review == s.review, "steps re-ran and changed state"
    chats_after_first_run = 4
    assert len(CHAT_CALLS) == chats_after_first_run, \
        f"resume consulted the model {len(CHAT_CALLS) - chats_after_first_run} extra times"
    print("  ok   the human's approval resumes the SAME flow to done — no step re-ran")

    srv.shutdown()
    print("agent-flow test: ok")


if __name__ == "__main__":
    main()
