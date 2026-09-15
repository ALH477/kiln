#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""serve_test.py — serve.py's HTTP surface, no model, no CrewAI.

Proves worktree_allowed, 409 on a busy task, 429 past MAX_PARALLEL, and
that a decision while the lock is held does not claim resumed:true.
"""

import json
import os
import sys
import tempfile
import threading
import time
from http.server import ThreadingHTTPServer
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from kiln_agents import serve  # noqa: E402


def _post(port, path, body):
    req = Request(f"http://127.0.0.1:{port}{path}", method="POST",
                  data=json.dumps(body).encode(),
                  headers={"Content-Type": "application/json"})
    try:
        with urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read() or b"{}")
    except HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def main():
    repo = Path(tempfile.mkdtemp(prefix="kiln-serve-"))
    wt_root = repo / ".studio" / "agents" / "worktrees"
    good = wt_root / "t1"
    good.mkdir(parents=True)
    os.environ["KILN_REPO"] = str(repo)

    assert serve.worktree_allowed(str(good), str(repo))
    assert not serve.worktree_allowed(str(repo), str(repo))
    assert not serve.worktree_allowed("/tmp", str(repo))
    assert not serve.worktree_allowed(str(good), "")
    print("  ok   worktree_allowed: managed dir only")

    held = threading.Event()
    release = threading.Event()

    def fake_run(task_id, brief=None, worktree=None, approved=None):
        lock = serve._task_lock(task_id)
        if not lock.acquire(blocking=False):
            serve._slots.release()
            return
        held.set()
        release.wait(timeout=5)
        lock.release()
        serve._slots.release()

    serve._run = fake_run
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), serve.Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    port = httpd.server_address[1]

    st, body = _post(port, "/task", {"task_id": "a", "brief": "x", "worktree": str(good)})
    assert st == 202 and body.get("running"), (st, body)
    assert held.wait(timeout=2), "fake_run did not take the lock"
    st, body = _post(port, "/task/a/decision", {"approved": True})
    assert st == 409 and body.get("resumed") is not True, (st, body)
    print("  ok   decision on a busy task is 409, not resumed")

    good2 = wt_root / "t2"
    good2.mkdir()
    good3 = wt_root / "t3"
    good3.mkdir()
    st, _ = _post(port, "/task", {"task_id": "b", "brief": "x", "worktree": str(good2)})
    assert st == 202, st
    st, body = _post(port, "/task", {"task_id": "c", "brief": "x", "worktree": str(good3)})
    assert st == 429, (st, body)
    print("  ok   third concurrent task is 429")

    st, body = _post(port, "/task", {"task_id": "d", "brief": "x", "worktree": str(repo)})
    assert st == 400 and "worktree" in body.get("error", ""), (st, body)
    print("  ok   a worktree outside the managed dir is refused")

    release.set()
    httpd.shutdown()
    print("serve test: ok")


if __name__ == "__main__":
    main()
