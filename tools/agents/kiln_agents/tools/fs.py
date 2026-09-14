# SPDX-License-Identifier: MIT
"""fs.py — worktree-scoped file tools: read, write, exact-string edit, list, grep.

BaseTool subclasses with pydantic args, CrewAI's own idiom. Every path goes
through _scope.in_worktree. Writes go through a temp file + os.replace, the
same discipline the studio's save endpoint uses, so a half-written file is
never what a gate reads.
"""

import os
import re
from pathlib import Path

from crewai.tools import BaseTool
from pydantic import BaseModel, Field

from ._scope import in_worktree

MAX_READ = 120_000   # characters of a file a tool returns
MAX_WRITE = 400_000  # bytes a single WriteFile accepts
MAX_GREP = 200       # matches returned


class _WorktreeArgs(BaseModel):
    path: str = Field(description="path relative to the task worktree")


class ReadFileArgs(_WorktreeArgs):
    pass


class ReadFile(BaseTool):
    name: str = "read_file"
    description: str = "Read a text file from the task worktree (truncated past ~120k characters)."
    args_schema: type[BaseModel] = ReadFileArgs
    worktree: str = ""

    def _run(self, path: str) -> str:
        p = in_worktree(self.worktree, path)
        if not p.is_file():
            return f"no such file: {path}"
        text = p.read_text(errors="replace")
        if len(text) > MAX_READ:
            return text[:MAX_READ] + f"\n[... truncated; {len(text)} characters total]"
        return text


class WriteFileArgs(_WorktreeArgs):
    content: str = Field(description="the file's full new content")


class WriteFile(BaseTool):
    name: str = "write_file"
    description: str = ("Write a file in the task worktree (creates parents). For a change to an "
                        "existing file prefer edit_file — a full rewrite you got slightly wrong "
                        "destroys what was there.")
    args_schema: type[BaseModel] = WriteFileArgs
    worktree: str = ""

    def _run(self, path: str, content: str) -> str:
        if len(content.encode()) > MAX_WRITE:
            raise ValueError(f"refusing to write {len(content)} characters at once (cap {MAX_WRITE})")
        p = in_worktree(self.worktree, path)
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = p.with_name(p.name + ".kiln-agent-tmp")
        tmp.write_text(content)
        os.replace(tmp, p)
        return f"wrote {path} ({len(content)} characters)"


class EditFileArgs(_WorktreeArgs):
    old: str = Field(description="the exact text to replace; must occur exactly once")
    new: str = Field(description="the replacement text")


class EditFile(BaseTool):
    name: str = "edit_file"
    description: str = ("Replace one exact string in a worktree file. Fails unless `old` occurs "
                        "exactly once — re-read the file and widen the anchor rather than guessing.")
    args_schema: type[BaseModel] = EditFileArgs
    worktree: str = ""

    def _run(self, path: str, old: str, new: str) -> str:
        p = in_worktree(self.worktree, path)
        if not p.is_file():
            return f"no such file: {path}"
        text = p.read_text(errors="replace")
        n = text.count(old)
        if n != 1:
            return (f"edit refused: `old` occurs {n} times in {path} (must be exactly 1); "
                    "read the file and widen the anchor")
        p.write_text(text.replace(old, new, 1))
        return f"edited {path} ({len(old)} -> {len(new)} characters)"


class ListDirArgs(_WorktreeArgs):
    pass


class ListDir(BaseTool):
    name: str = "list_dir"
    description: str = "List a directory in the task worktree, one entry per line (dirs end in /)."
    args_schema: type[BaseModel] = ListDirArgs
    worktree: str = ""

    def _run(self, path: str) -> str:
        p = in_worktree(self.worktree, path)
        if not p.is_dir():
            return f"no such directory: {path}"
        out = []
        for child in sorted(p.iterdir()):
            out.append(child.name + ("/" if child.is_dir() else ""))
            if len(out) >= 500:
                out.append("[...]")
                break
        return "\n".join(out) or "(empty)"


class GrepArgs(BaseModel):
    pattern: str = Field(description="a regular expression")
    path: str = Field(default=".", description="directory or file in the worktree to search")
    glob: str = Field(default="", description="limit to files matching this glob, e.g. '*.c'")


class Grep(BaseTool):
    name: str = "grep"
    description: str = "Search the worktree for a regex; returns `path:line: text` matches."
    args_schema: type[BaseModel] = GrepArgs
    worktree: str = ""

    def _run(self, pattern: str, path: str = ".", glob: str = "") -> str:
        base = in_worktree(self.worktree, path)
        rx = re.compile(pattern)
        files = [base] if base.is_file() else sorted(
            p for p in base.rglob(glob or "*") if p.is_file())
        out, root = [], Path(self.worktree).resolve()
        for f in files:
            try:
                for i, line in enumerate(f.read_text(errors="replace").splitlines(), 1):
                    if rx.search(line):
                        out.append(f"{f.relative_to(root)}:{i}: {line.strip()[:200]}")
                        if len(out) >= MAX_GREP:
                            out.append(f"[... capped at {MAX_GREP} matches]")
                            return "\n".join(out)
            except (OSError, UnicodeError):
                continue
        return "\n".join(out) or "no matches"


def file_tools(worktree: str) -> list[BaseTool]:
    return [t(worktree=worktree) for t in (ReadFile, WriteFile, EditFile, ListDir, Grep)]
