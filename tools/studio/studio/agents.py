# SPDX-License-Identifier: MIT
"""agents.py — the studio side of the agent loop: tasks, worktrees, merges.

The STUDIO owns the worktree lifecycle, not the agent (a runtime that can
create its own worktrees can lose them; a studio that can creates them, shows
their diff, and merges or deletes). One task = one worktree at
``.studio/agents/worktrees/<task_id>`` on branch ``agent/<task_id>``, a record in
``.studio/agents/tasks.json``, and a flow run in the agents container kicked
over HTTP (KILN_AGENTS_URL).

Records keep: brief, who asked (a human via the panel, or the mesh later),
phase (patched by the flow as it runs), the plan/review/validations the flow
reports, the human's decision, and the eventual merge outcome. The flow's
PATCH is whitelisted — it can report, never decide; `approved` moves only
through decide(), called from the approve/reject endpoint a person clicks.

Merge is fast-forward-or-refuse: the flow's branch must contain the served
checkout's HEAD exactly. A branch that has drifted behind work the humans
did is refused, never silently merged.
"""

import json
import os
import subprocess
import threading
import time
import urllib.request
from pathlib import Path

PATCHABLE = {"phase", "plan", "kind", "validations", "review", "merge_request", "report"}


class AgentTasks:
    def __init__(self, repo: Path):
        self.repo = repo
        self.dir = repo / ".studio" / "agents"
        self.dir.mkdir(parents=True, exist_ok=True)
        self.file = self.dir / "tasks.json"
        self.lock = threading.Lock()
        self.tasks = self._load()
        self.url = os.environ.get("KILN_AGENTS_URL", "").rstrip("/")

    # ── storage ───────────────────────────────────────────────────────────
    def _load(self) -> dict:
        if self.file.is_file():
            try:
                return json.loads(self.file.read_text())
            except (OSError, json.JSONDecodeError):
                pass
        return {}

    def _save(self):
        tmp = self.file.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.tasks, indent=1) + "\n")
        os.replace(tmp, self.file)

    def _git(self, *argv, cwd=None, check=True):
        proc = subprocess.run(["git", *argv], cwd=cwd or self.repo,
                              capture_output=True, text=True, timeout=120)
        if check and proc.returncode != 0:
            raise ValueError(proc.stderr.strip() or f"git {' '.join(argv)} failed")
        return proc.stdout.strip()

    def _call_agents(self, method: str, path: str, body: dict | None = None):
        if not self.url:
            raise ValueError("no agents service configured (KILN_AGENTS_URL unset)")
        req = urllib.request.Request(
            self.url + path, method=method,
            data=json.dumps(body).encode() if body is not None else None,
            headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=15) as r:
            return json.loads(r.read() or b"{}")

    # ── the lifecycle ─────────────────────────────────────────────────────
    def create(self, brief: str, by: str) -> dict:
        brief = brief.strip()
        if not brief:
            raise ValueError("a task needs a brief")
        task_id = f"t{int(time.time()) % 10_000_000:07d}-{os.urandom(3).hex()}"
        branch = f"agent/{task_id}"
        worktree = self.dir / "worktrees" / task_id
        worktree.parent.mkdir(parents=True, exist_ok=True)
        self._git("worktree", "add", str(worktree), "-b", branch)
        rec = {"id": task_id, "brief": brief[:2000], "by": by, "branch": branch,
               "worktree": str(worktree), "phase": "created", "approved": None,
               "created": int(time.time())}
        try:
            self._call_agents("POST", "/task",
                              {"task_id": task_id, "brief": brief, "worktree": str(worktree)})
            rec["phase"] = "running"
        except (ValueError, OSError) as e:
            rec["phase"] = "queued"
            rec["note"] = f"agents service not reached ({e}); the task and its worktree exist — " \
                          "start the agents container and PATCH it, or delete and retry"
        with self.lock:
            self.tasks[task_id] = {k: v for k, v in rec.items()}
            self._save()
        return rec

    def patch(self, task_id: str, fields: dict) -> dict:
        """The flow reporting in. Whitelisted keys only — a flow reports, it
        never decides (approved) and never deletes (worktree)."""
        with self.lock:
            rec = self.tasks.get(task_id)
            if rec is None:
                raise KeyError(task_id)
            for k, v in fields.items():
                if k in PATCHABLE:
                    rec[k] = v
            self._save()
            return rec

    def decide(self, task_id: str, approved: bool, by: str) -> dict:
        """A person's yes/no. Recorded, then relayed to the agents service,
        which re-kicks the persisted flow so await_human → finish completes."""
        with self.lock:
            rec = self.tasks.get(task_id)
            if rec is None:
                raise KeyError(task_id)
            if rec.get("approved") is not None:
                raise ValueError("already decided")
            rec["approved"] = bool(approved)
            rec["decided_by"] = by
            self._save()
        try:
            self._call_agents("POST", f"/task/{task_id}/decision", {"approved": approved})
        except (ValueError, OSError) as e:
            with self.lock:
                rec["note"] = f"decision recorded; the agents service was not reached ({e}) — " \
                              "the flow resumes when it is"
                self._save()
        return rec

    def merge(self, task_id: str) -> dict:
        """Fast-forward the served checkout to the agent branch, or refuse.
        On success the worktree goes away and the branch is deleted — a
        merged task leaves no residue."""
        with self.lock:
            rec = self.tasks.get(task_id)
            if rec is None:
                raise KeyError(task_id)
            if rec.get("approved") is not True:
                raise ValueError("merge needs an approval first")
            if rec.get("phase") not in ("done",):
                raise ValueError(f"the flow is not done (phase: {rec.get('phase')})")
        branch = rec["branch"]
        self._git("merge", "--ff-only", branch)  # raises when not a fast-forward
        self._git("worktree", "remove", "--force", rec["worktree"], check=False)
        self._git("branch", "-D", branch, check=False)
        with self.lock:
            rec["merged"] = True
            rec["phase"] = "merged"
            self._save()
        return rec

    def discard(self, task_id: str) -> dict:
        with self.lock:
            rec = self.tasks.get(task_id)
            if rec is None:
                raise KeyError(task_id)
        self._git("worktree", "remove", "--force", rec["worktree"], check=False)
        self._git("branch", "-D", rec["branch"], check=False)
        with self.lock:
            rec["discarded"] = True
            rec["phase"] = "discarded"
            self._save()
        return rec

    def get(self, task_id: str, with_diff=True) -> dict | None:
        with self.lock:
            rec = dict(self.tasks.get(task_id) or {})
        if not rec:
            return None
        if with_diff and Path(rec.get("worktree", "/nonexistent")).is_dir():
            rec["diff_stat"] = self._git("diff", "--stat", "HEAD", check=False,
                                         cwd=rec["worktree"])
            base = self._git("rev-parse", "HEAD", check=False)
            if base:
                rec["ahead"] = self._git("log", "--oneline", f"{base}..HEAD",
                                         check=False, cwd=rec["worktree"])
        rec["agents_service"] = bool(self.url)
        return rec

    def list(self) -> list:
        with self.lock:
            return sorted((dict(r) for r in self.tasks.values()),
                          key=lambda r: r.get("created", 0), reverse=True)
