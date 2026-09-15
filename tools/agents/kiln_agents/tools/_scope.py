# SPDX-License-Identifier: MIT
"""_scope.py — the one rule every agent tool enforces: the task worktree.

An agent edits only its own worktree (created BY THE STUDIO at
`.studio/worktrees/<task>`); the main tree is for humans. Every path a tool
touches goes through `in_worktree`, which resolves symlinks and refuses
anything outside the root. Refusal raises ValueError — CrewAI turns a raised
tool into feedback the agent sees, so a wandering agent is told, not obeyed.
"""

from pathlib import Path


def in_worktree(worktree: str | Path, path: str) -> Path:
    root = Path(worktree).resolve()
    p = (root / path).resolve() if not Path(path).is_absolute() else Path(path).resolve()
    try:
        rel = p.relative_to(root)
    except ValueError:
        raise ValueError(f"{path!r} is outside the task worktree {root} — refused")
    if rel.parts and rel.parts[0] == ".git":
        raise ValueError(f"{path!r} names the worktree gitdir — refused")
    return p
