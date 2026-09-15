# SPDX-License-Identifier: MIT
"""flow.py — KilnTaskFlow: one CrewAI Flow per task, from brief to merge request.

    intake → plan → route (content | engine | docs)
          → work → validate ─ red, rounds left ─→ work (the same listeners)
                   │
                   └ green (or out of rounds) → review → await_human → finish

Two properties the shape above buys, and both are why CrewAI alone is not the
answer:

- **@persist means approval survives time.** `await_human` publishes the merge
  request and the flow is DONE running; serve.py re-kicks the same flow id
  when the studio records the human's yes/no. CrewAI restarts a persisted
  flow from its `@start`, so every step guards on its downstream artifact
  (`if self.state.review: return` and friends) — a re-kick must walk to
  `finish` without redoing the plan, the edits, or the gates. agent-flow's
  check drives exactly that double run.

- **The deterministic parts are code, not model.** Routing is a named kind,
  validation verdicts are the studio reports' own `ok`, the loop is capped at
  MAX_ROUNDS. Models are consulted only where judgement is the work:
  planning, authoring, reviewing. A check a robot can run, a robot runs —
  through the studio, whose job semaphore is how agents and two humans share
  build capacity.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
from pathlib import Path

import yaml  # pyyaml rides in with crewai
from crewai.flow.flow import Flow, listen, or_, router, start
from crewai.flow.persistence import persist
from pydantic import BaseModel, Field

from . import models

MAX_ROUNDS = 3
AGENTS_YAML = Path(__file__).parent / "agents.yaml"


def flow_db_path() -> str:
    """Where flow state persists. KILN_AGENTS_DB wins; the default keeps task
    state beside the studio's own state under $KILN_REPO/.studio/agents/."""
    db = Path(os.environ.get("KILN_AGENTS_DB",
                             Path(os.environ.get("KILN_REPO", ".")) / ".studio" / "agents"
                             / "flows.db"))
    db.parent.mkdir(parents=True, exist_ok=True)
    return str(db)


# ONE instance, deliberately module-level: crewai's @persist bakes its
# persistence argument into per-method SAVE closures at class-decoration
# time, while a persistence passed to __init__ is used only for LOADS —
# give the constructor a different instance and the flow saves to one
# database and resumes from another, silently (found by the agent-flow
# check: an empty flow_states table next to a completed run). Everything
# that builds a KilnTaskFlow passes this object; KILN_AGENTS_DB chooses
# its path and must be set before this module is imported.
from crewai.flow.persistence.sqlite import SQLiteFlowPersistence

FLOW_PERSISTENCE = SQLiteFlowPersistence(db_path=flow_db_path())


class TaskState(BaseModel):
    task_id: str = ""
    brief: str = ""
    worktree: str = ""
    kind: str = ""                       # content | engine | docs; the planner names it
    plan: str = ""
    rounds: int = 0                      # validate→work loops so far
    validations: list[dict] = Field(default_factory=list)
    validation_ok: bool = False
    checks: list[str] = Field(default_factory=list)     # checks the plan named
    review: str = ""
    approved: bool | None = None         # None while a human has not decided
    report: dict = Field(default_factory=dict)
    base_sha: str = ""                   # HEAD at intake; reviewer diffs this...HEAD


def _agent(role: str, worktree: str, base: str = ""):
    from crewai import Agent
    from .tools import role_tools
    conf = yaml.safe_load(AGENTS_YAML.read_text())[role]
    # The mesh, when the container has one: nix/studio-module.nix points
    # KILN_MESH_URL at HydraMesh's mesh_mcp (tools: mesh_send/recv/inbox/
    # status) over the compose network. Attached as a STRUCTURED
    # MCPServerHTTP config — the plain-string form only accepts https or a
    # slug, and an http docker-network URL is neither. Unset (offline gates,
    # local runs) and no agent ever dials it.
    mesh_url = os.environ.get("KILN_MESH_URL", "")
    mcps = None
    if mesh_url:
        from crewai.mcp.config import MCPServerHTTP
        mcps = [MCPServerHTTP(url=mesh_url, cache_tools_list=True)]
    return Agent(role=conf["role"].strip(), goal=conf["goal"].strip(),
                 backstory=conf["backstory"].strip(),
                 llm=models.llm_for(role), tools=role_tools(role, worktree, base),
                 mcps=mcps, max_rpm=20, share_crew=False)


def _kick(role: str, worktree: str, description: str, expected: str, base: str = "") -> str:
    """One agent on one task. memory=False and planning=False are load-bearing:
    CrewAI's defaults for both reach for OpenAI, and there is no OpenAI key."""
    from crewai import Crew, Task
    agent = _agent(role, worktree, base)
    crew = Crew(agents=[agent], tasks=[Task(description=description,
                                            expected_output=expected, agent=agent)],
                share_crew=False, memory=False, planning=False)
    return str(crew.kickoff())


# NB: @persist() with the parens — persist is a decorator FACTORY
# (persist(persistence=None, verbose=False)); a bare @persist would hand the
# flow class itself in as the persistence backend.
@persist(persistence=FLOW_PERSISTENCE)
class KilnTaskFlow(Flow[TaskState]):
    # Flow._flow_post_init auto-creates a Memory the moment memory is None
    # (crewai/flow/flow.py:1124), and Memory's default storage is lancedb —
    # struck out of this env on purpose (see nix/agent-python.nix) — plus its
    # default embedder is OpenAI, which has no key here. Same off switch
    # crewai's own internal flows use (experimental/agent_executor.py).
    _skip_auto_memory = True

    def _post_task_status(self, patch: dict):
        """Best-effort: keep the studio's task record in step with the flow."""
        from .tools.studiojob import studio_call
        try:
            studio_call("PATCH", f"/api/agent-tasks/{self.state.task_id}", patch)
        except (ValueError, OSError):
            pass  # the studio being unreachable pauses reporting, not the task

    @start()
    def intake(self):
        if not self.state.brief.strip():
            raise ValueError("KilnTaskFlow needs state.brief")
        if not self.state.worktree or not Path(self.state.worktree).is_dir():
            raise ValueError("KilnTaskFlow needs state.worktree (created by the studio)")
        if not self.state.base_sha:
            proc = subprocess.run(["git", "-C", self.state.worktree, "rev-parse", "HEAD"],
                                  capture_output=True, text=True, timeout=30)
            self.state.base_sha = proc.stdout.strip()

    @listen(intake)
    def plan(self):
        if self.state.plan:  # re-kicked after the human decided: never re-plan
            return
        listing = "\n".join(sorted(os.listdir(self.state.worktree)))
        self.state.plan = _kick(
            "planner", self.state.worktree,
            description=(
                f"Task: {self.state.brief}\n\n"
                f"The worktree (a checkout of the Kiln repo) contains at its top level:\n{listing}\n\n"
                "Write the plan: which files change, the task's kind, and the flake checks or "
                "studio validators that prove the change. End with two lines, exactly:\n"
                "kind: <content|engine|docs>\n"
                "checks: <comma-separated check or validator:path names, or 'none'>"),
            expected="a short plan ending in the `kind:` and `checks:` lines",
            base=self.state.base_sha)
        m = re.search(r"^kind:\s*(\w+)", self.state.plan, re.M)
        self.state.kind = m.group(1) if m and m.group(1) in ("content", "engine", "docs") else "docs"
        m = re.search(r"^checks:\s*(.+)$", self.state.plan, re.M)
        if m and m.group(1).strip().lower() != "none":
            self.state.checks = [c.strip() for c in m.group(1).split(",") if c.strip()]
        self._post_task_status({"phase": "working", "plan": self.state.plan,
                                "kind": self.state.kind})

    @router(plan)
    def route(self):
        return self.state.kind

    @listen("content")
    def work_content(self):
        self._work("content")  # the ROLE names (agents.yaml keys) — the task
        # KINDs the router emits are "content"/"engine"/"docs", and "engine"
        # is not a role: engineering and docs both edit like the engineer.

    @listen("engine")
    def work_engine(self):
        self._work("engineer")

    @listen("docs")
    def work_docs(self):
        self._work("engineer")  # docs changes are still code-shaped edits

    def _work(self, role: str):
        if self.state.approved is not None or self.state.review:
            return  # a re-kick after the human decided must never redo the edits
        feedback = ""
        if self.state.validations and not self.state.validation_ok:
            feedback = ("\n\nThe previous attempt's validation FAILED. The reports:\n"
                        + json.dumps(self.state.validations[-2:], indent=2)[:6000])
        _kick(
            role, self.state.worktree,
            description=(
                f"Task: {self.state.brief}\n\nThe plan:\n{self.state.plan}\n{feedback}\n\n"
                "Do the work in this worktree. Then stage and commit it on the current branch "
                "with the git tool. When the task is a large code change and you have the "
                "claude_worker tool, you may delegate it."),
            expected="a summary of the change, ending with the commit id",
            base=self.state.base_sha)

    # Three thin listeners, NOT one or_(work_*): crewai's engine fires a
    # multi-trigger or_() listener ONCE per run (_fired_or_listeners), which
    # would silently kill the validate→work→validate loop after round one.
    # Single-source listeners fire every time; each wrapper calls the shared
    # body. (The stacked @listen(work_*) decorators this replaces collapsed
    # to the LAST condition anyway — crewai keeps one listener entry per
    # method, not a set.)
    @listen(work_content)
    def validate_content(self):
        self._validate()

    @listen(work_engine)
    def validate_engine(self):
        self._validate()

    @listen(work_docs)
    def validate_docs(self):
        self._validate()

    def _validate(self):
        if self.state.approved is not None or self.state.review:
            return
        self.state.rounds += 1
        results = []
        if not self.state.checks:
            self.state.validation_ok = True
            results.append({"name": "(none named)", "ok": True,
                            "error": "plan named no check — the human review decides"})
        else:
            from .tools.studiojob import studio_call, wait_job
            for name in self.state.checks:
                try:
                    validator, sep, arg = name.partition(":")
                    if sep:  # "map-validate:assets/foo.map" — a validator over a path
                        job = studio_call("POST", "/api/jobs",
                                          {"kind": "validate", "target": validator, "arg": arg})
                    else:
                        job = studio_call("POST", "/api/jobs", {"kind": "check", "target": name})
                    settled = wait_job(job["id"])
                    rep = settled.get("report") or {}
                    results.append({"name": name, "state": settled.get("state"),
                                    "ok": bool(rep.get("ok")) if rep else settled.get("exit") == 0,
                                    "report": rep})
                except (ValueError, OSError) as e:
                    results.append({"name": name, "ok": False, "error": str(e)[:300]})
            self.state.validation_ok = all(r.get("ok") for r in results)
        self.state.validations.extend(results)
        self._post_task_status({"phase": "validating", "validations": self.state.validations})

    # or_() is safe on a ROUTER: crewai's fire-once rule tracks listeners, and
    # routers are explicitly exempt ("routers fire every time").
    @router(or_(validate_content, validate_engine, validate_docs))
    def verdict(self):
        if self.state.validation_ok:
            return "to_review"
        if self.state.rounds < MAX_ROUNDS:
            return self.state.kind  # re-enter the same work listeners, failure in context
        return "to_review"       # out of rounds: a human sees the red branch and decides

    # NB "to_review" and not "review": a router outcome fires the listener whose
    # label it matches, and a METHOD firing re-emits its own name — an outcome
    # named like its listener is a self-listen, crewai's max_method_calls
    # RecursionError ("a @listen label matches the method's own name").
    @listen("to_review")
    def review(self):
        if self.state.review:
            return
        spec = f"{self.state.base_sha}...HEAD" if self.state.base_sha else "HEAD"
        diff = subprocess.run(["git", "-C", self.state.worktree, "diff", "--stat", spec],
                              capture_output=True, text=True, timeout=60).stdout.strip()
        self.state.review = _kick(
            "reviewer", self.state.worktree,
            description=(
                f"Task: {self.state.brief}\nPlan:\n{self.state.plan}\n\n"
                f"Diff stat:\n{diff}\n\n"
                f"Validation results: {json.dumps(self.state.validations, indent=2)[:4000]}\n\n"
                "Read the actual diff with the git tool and the changed files. Then deliver a "
                "verdict: APPROVE or BLOCK, with one line of reasoning per point."),
            expected="a verdict of APPROVE or BLOCK with reasoning",
            base=self.state.base_sha)
        self._post_task_status({"phase": "review", "review": self.state.review})

    @listen(review)
    def await_human(self):
        """Publish the merge request and stop running. serve.py re-kicks this
        flow id when the studio records the human's decision; nothing sleeps
        in memory for hours."""
        self._post_task_status({"phase": "awaiting-human", "merge_request": True})

    @listen(await_human)
    def finish(self):
        if self.state.approved is None:
            return  # first pass: the human has not decided; the re-kick resumes here
        self.state.report = {
            "task_id": self.state.task_id, "kind": self.state.kind,
            "plan": self.state.plan[-2000:], "rounds": self.state.rounds,
            "validations": self.state.validations, "review": self.state.review[-2000:],
            "approved": self.state.approved,
        }
        self._post_task_status({"phase": "done", "approved": self.state.approved,
                                "report": self.state.report})
