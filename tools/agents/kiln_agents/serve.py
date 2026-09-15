# SPDX-License-Identifier: MIT
"""serve.py — the agents container's task loop.

A small HTTP service (stdlib ThreadingHTTPServer — no web framework, same
discipline as tools/studio) bound to the docker network only, next to the
studio container. Two endpoints:

  POST /task        {"task_id", "brief", "worktree"}  → run the flow in a
                      thread; the studio calls this when a person (or the
                      mesh) submits a task. Idempotent per task_id.
  POST /task/<id>/decision  {"approved": true|false}  → re-kick the persisted
                      flow so `await_human` → `finish` completes. The studio
                      calls this when a person clicks approve or reject.
  GET  /healthz     → {"ok": true}

Concurrency: one flow run per task_id at a time (a lock map), several tasks
in parallel at most MAX_PARALLEL — agent work is model-latency-bound, not
CPU-bound, but the budget is the Ollama subscription's metering, so parallel
task count is a real knob.

Worktrees must sit under $KILN_REPO/.studio/agents/worktrees/ — the studio
creates them there; a caller that points at the served checkout is refused.
"""

from __future__ import annotations

import json
import os
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

MAX_PARALLEL = 2
MAX_BODY = 1_000_000
_locks: dict[str, threading.Lock] = {}
_locks_guard = threading.Lock()
_slots = threading.Semaphore(MAX_PARALLEL)


def worktree_allowed(worktree: str, repo: str | None = None) -> bool:
    """True iff worktree is a directory under <repo>/.studio/agents/worktrees/."""
    root_s = repo if repo is not None else os.environ.get("KILN_REPO", "")
    if not root_s or not worktree:
        return False
    try:
        wt = Path(worktree).resolve()
        root = (Path(root_s).resolve() / ".studio" / "agents" / "worktrees")
        wt.relative_to(root)
    except (ValueError, OSError):
        return False
    return wt.is_dir()


def _task_lock(task_id: str) -> threading.Lock:
    with _locks_guard:
        return _locks.setdefault(task_id, threading.Lock())


def _run(task_id: str, brief: str | None, worktree: str | None, approved=None):
    # Imported here so serve.py can be tested without pulling CrewAI.
    from .flow import FLOW_PERSISTENCE, KilnTaskFlow
    lock = _task_lock(task_id)
    if not lock.acquire(blocking=False):
        _slots.release()
        print(f"serve: task {task_id} already running; ignoring duplicate kick")
        return
    try:
        # FLOW_PERSISTENCE, not a fresh instance: the @persist decorator saves
        # through the module-level object — a different instance here would
        # load from a db nothing was saved to (see flow.py's note).
        flow = KilnTaskFlow(persistence=FLOW_PERSISTENCE)
        inputs = {"id": task_id}
        if brief is not None:
            inputs.update(task_id=task_id, brief=brief, worktree=worktree)
        if approved is not None:
            inputs["approved"] = approved
        flow.kickoff(inputs=inputs)
        print(f"serve: task {task_id} kick done")
    except Exception as e:  # a crashed flow must not take the service down
        print(f"serve: task {task_id} crashed: {e}")
    finally:
        lock.release()
        _slots.release()


class Handler(BaseHTTPRequestHandler):
    server_version = "kiln-agents/1"

    def log_message(self, *a):
        pass

    def _json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/healthz":
            return self._json({"ok": True})
        self._json({"error": "not found"}, 404)

    def do_POST(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            return self._json({"error": "bad content-length"}, 400)
        if n < 0 or n > MAX_BODY:
            return self._json({"error": "body too large"}, 413)
        try:
            body = json.loads(self.rfile.read(n) or b"{}")
        except json.JSONDecodeError:
            return self._json({"error": "bad json"}, 400)

        if self.path == "/task":
            task_id, brief, worktree = body.get("task_id"), body.get("brief"), body.get("worktree")
            if not task_id or not brief or not worktree:
                return self._json({"error": "task needs task_id, brief, worktree"}, 400)
            if not worktree_allowed(str(worktree)):
                return self._json({"error": "worktree must be under $KILN_REPO/.studio/agents/worktrees/"}, 400)
            lock = _task_lock(str(task_id))
            if lock.locked():
                return self._json({"error": "task already running"}, 409)
            if not _slots.acquire(blocking=False):
                return self._json({"error": "too many parallel tasks"}, 429)
            threading.Thread(target=_run, args=(task_id, brief, worktree),
                             kwargs={"approved": None}, daemon=True).start()
            return self._json({"task_id": task_id, "running": True}, 202)

        if self.path.startswith("/task/") and self.path.endswith("/decision"):
            task_id = self.path[len("/task/"):-len("/decision")]
            if "approved" not in body:
                return self._json({"error": "decision needs approved: true|false"}, 400)
            lock = _task_lock(task_id)
            if lock.locked():
                return self._json({"error": "task already running"}, 409)
            if not _slots.acquire(blocking=False):
                return self._json({"error": "too many parallel tasks"}, 429)
            threading.Thread(target=_run, args=(task_id, None, None),
                             kwargs={"approved": bool(body["approved"])}, daemon=True).start()
            return self._json({"task_id": task_id, "resumed": True}, 202)

        self._json({"error": "not found"}, 404)


def main():
    host, port = "0.0.0.0", int(os.environ.get("KILN_AGENTS_PORT", "8600"))
    print(f"kiln-agents serving on {host}:{port} (docker network only; no auth beyond that)")
    ThreadingHTTPServer((host, port), Handler).serve_forever()


if __name__ == "__main__":
    main()
