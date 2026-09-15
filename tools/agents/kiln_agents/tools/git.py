# SPDX-License-Identifier: MIT
"""git.py — the Git tool: the task branch only, never a push.

An agent's git licence is: look (status/diff/log), stage and commit ON THE
TASK BRANCH. `push`, `checkout`/`switch`, `reset`, `rebase`, force flags and
any pathspec naming something outside the worktree are refused. Everything
runs as a fixed argv list with cwd = the worktree — no shell, ever.
"""

import subprocess

from crewai.tools import BaseTool
from pydantic import BaseModel, Field

_OPS = {
    "status": ["status", "--short", "--branch"],
    "diff":   ["diff", "--stat", "HEAD"],
    "log":    ["log", "--oneline", "-10"],
}


class GitArgs(BaseModel):
    op: str = Field(description="one of: status, diff, log, add, commit")
    paths: list[str] = Field(default_factory=list,
                             description="for add: worktree-relative paths to stage")
    message: str = Field(default="", description="for commit: the commit message")


class Git(BaseTool):
    name: str = "git"
    description: str = ("Git on the task branch: status / diff / log / add <paths> / "
                        "commit -m. Never pushes, never switches branches, never force-anything.")
    args_schema: type[BaseModel] = GitArgs
    worktree: str = ""

    def _git(self, argv: list[str]) -> str:
        proc = subprocess.run(["git", "-C", self.worktree, *argv],
                              capture_output=True, text=True, timeout=120)
        out = (proc.stdout + proc.stderr).strip()
        return out[:8000] or f"(exit {proc.returncode}, no output)"

    def _run(self, op: str, paths: list[str] | None = None, message: str = "") -> str:
        if op in _OPS:
            return self._git(_OPS[op])

        if op == "add":
            from ._scope import in_worktree
            if not paths:
                return "add needs paths"
            for p in paths:
                try:
                    in_worktree(self.worktree, p)  # refuses outside, incl. ../ escapes
                except ValueError as e:
                    return f"git add {p!r} {e}"  # a refusal is a tool answer, not a crash
            return self._git(["add", "--", *paths])

        if op == "commit":
            if not message.strip():
                return "commit needs a message"
            return self._git(["commit", "-m", message])

        return (f"git op {op!r} refused: allowed ops are status, diff, log, add, commit. "
                "Push and branch surgery are the human's and the studio's, never an agent's.")
