# SPDX-License-Identifier: MIT
"""claudeworker.py — hand a big code change to a Claude Code worker.

CrewAI role agents do small, well-scoped edits themselves. When a task is a
large code change, the engineer role hands it to `claude -p` — a non-
interactive Claude Code run pinned to the task worktree with a narrow tool
allowlist and a turn/time budget. The argv is FIXED here and asserted by
nix/checks/agent-tools.nix: prompt over stdin (never on the command line),
cwd = the worktree, edits auto-accepted within the allowed tools, every tool
an agent may not have (push, other branches, the main tree) simply absent
from the allowlist.

Returns the worker's final message plus the resulting `git diff --stat`.
"""

import json
import os
import shutil
import subprocess
import time

from crewai.tools import BaseTool
from pydantic import BaseModel, Field

ALLOWED_TOOLS = ("Read,Edit,Write,Grep,Glob,"
                 "Bash(nix build:*),Bash(./dev cheap),"
                 "Bash(./dev map-emit:*),Bash(./dev map-dump:*),"
                 "Bash(./dev map-validate:*),Bash(./dev map-canon:*),"
                 "Bash(./dev map-render:*),"
                 "Bash(git diff:*),Bash(git status:*)")

MAX_PROMPT = 60_000


class ClaudeWorkerArgs(BaseModel):
    brief: str = Field(description="the full brief for the worker: what to change, the gates to run, "
                                   "the shape of done. The worker sees only this plus the worktree.")
    budget_s: int = Field(default=600, description="wall-clock cap in seconds")


class ClaudeWorker(BaseTool):
    name: str = "claude_worker"
    description: str = ("Delegate a large code change to a Claude Code worker run in the task "
                        "worktree with a narrow tool allowlist and a time budget. Returns its "
                        "final message and the diff it produced.")
    args_schema: type[BaseModel] = ClaudeWorkerArgs
    worktree: str = ""
    claude_bin: str = ""  # default: shutil.which("claude")

    def _argv(self) -> list[str]:
        return [
            self.claude_bin or shutil.which("claude") or "claude",
            "-p",  # prompt arrives on stdin, never visible in ps
            "--output-format", "stream-json",
            "--permission-mode", "acceptEdits",
            "--allowedTools", ALLOWED_TOOLS,
        ]

    def _git(self, argv: list[str]) -> str:
        proc = subprocess.run(["git", "-C", self.worktree, *argv],
                              capture_output=True, text=True, timeout=60)
        return (proc.stdout + proc.stderr).strip()

    def _run(self, brief: str, budget_s: int = 600) -> str:
        budget_s = max(30, min(int(budget_s), 3600))
        if len(brief) > MAX_PROMPT:
            return f"brief too long ({len(brief)} > {MAX_PROMPT} characters); trim it"
        env = dict(os.environ)
        env.pop("CLAUDECODE", None)  # allow nesting from a Claude Code session
        started = time.monotonic()
        try:
            proc = subprocess.run(
                self._argv(), input=brief, cwd=self.worktree,
                capture_output=True, text=True, timeout=budget_s, env=env)
        except subprocess.TimeoutExpired:
            return json.dumps({"ok": False, "error": f"worker exceeded its {budget_s}s budget",
                               "diff_stat": self._git(["diff", "--stat", "HEAD"])})
        except FileNotFoundError:
            return json.dumps({"ok": False, "error": "no `claude` on PATH in this container"})

        final = ""
        for line in proc.stdout.splitlines():
            try:
                evt = json.loads(line)
            except json.JSONDecodeError:
                continue
            if evt.get("type") == "result":
                final = evt.get("result", "")
        return json.dumps({
            "ok": proc.returncode == 0,
            "elapsed_s": round(time.monotonic() - started, 1),
            "final": final[-4000:] if final else proc.stdout[-2000:],
            "stderr_tail": proc.stderr[-1000:],
            "diff_stat": self._git(["diff", "--stat", "HEAD"]),
        })
