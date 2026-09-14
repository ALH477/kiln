# SPDX-License-Identifier: MIT
"""kiln_agents.tools — every tool the role agents get, and the per-role attach table."""

from .claudeworker import ClaudeWorker
from .fs import EditFile, Grep, ListDir, ReadFile, WriteFile, file_tools
from .git import Git
from .studiojob import StudioJob

__all__ = ["ClaudeWorker", "EditFile", "Grep", "Git", "ListDir", "ReadFile",
           "StudioJob", "WriteFile", "file_tools", "role_tools"]


def role_tools(role: str, worktree: str) -> list:
    """The toolset each role gets. Decided in code — where agent-tools.nix can
    assert it — not in yaml. No role gets a general shell; the studio and the
    scoped file/git surface are deliberately the whole world."""
    files = file_tools(worktree)
    git = [Git(worktree=worktree)]
    studio = [StudioJob()]
    return {
        "planner":   [ReadFile(worktree=worktree), ListDir(worktree=worktree),
                      Grep(worktree=worktree), *studio],
        "engineer":  [*files, *git, *studio, ClaudeWorker(worktree=worktree)],
        "content":   [*files, *git, *studio],
        "validator": [ReadFile(worktree=worktree), Grep(worktree=worktree), *studio],
        "reviewer":  [ReadFile(worktree=worktree), Grep(worktree=worktree), *git, *studio],
    }[role]
