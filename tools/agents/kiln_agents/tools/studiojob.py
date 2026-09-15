# SPDX-License-Identifier: MIT
"""studiojob.py — builds, checks and validators THROUGH the studio, never around it.

The studio is the authority on how a build or a check is invoked (job kinds
build their own argv; validators accept only paths they advertise), and its
job semaphore is how agent work shares build capacity with the two humans. An
agent that shelled out to `nix build` itself would bypass both. So this tool
is a thin client of the studio's HTTP API — the same calls
tools/studio/mcp_server.py exposes over MCP for Claude Code workers.

The studio base URL and token come from the environment
(KILN_STUDIO_URL / KILN_STUDIO_TOKEN, or the token file under $KILN_REPO), set
by the agents container's environment. There is no flag that turns this into
a local nix invocation.
"""

import json
import os
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from crewai.tools import BaseTool
from pydantic import BaseModel, Field

KINDS = ("build", "check", "validate", "host-shot", "map-render")


def _studio_base_and_token() -> tuple[str, str]:
    base = os.environ.get("KILN_STUDIO_URL", "").rstrip("/")
    token = os.environ.get("KILN_STUDIO_TOKEN", "")
    if not token:
        repo = Path(os.environ.get("KILN_REPO", "."))
        for name in ("agents-token", "token"):
            tf = repo / ".studio" / name
            if tf.is_file():
                token = tf.read_text().strip()
                if token:
                    break
    if not base or not token:
        raise ValueError("KILN_STUDIO_URL / KILN_STUDIO_TOKEN (or $KILN_REPO/.studio/agents-token) not set")
    return base, token


def studio_call(method: str, path: str, body: dict | None = None) -> dict:
    base, token = _studio_base_and_token()
    origin = urllib.parse.urlsplit(base)
    req = urllib.request.Request(
        base + path, method=method,
        data=json.dumps(body).encode() if body is not None else None,
        headers={"Authorization": f"Bearer {token}",
                 "Content-Type": "application/json",
                 # the studio's CSRF guard refuses an origin-less POST
                 "Origin": f"{origin.scheme}://{origin.netloc}"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            raw = r.read()
            return json.loads(raw) if raw.strip() else {}
    except urllib.error.HTTPError as e:
        detail = e.read().decode(errors="replace")[:300]
        raise ValueError(f"studio answered {e.code} for {path}: {detail}")


def wait_job(job_id: str, timeout_s: int = 900) -> dict:
    import time
    deadline = time.monotonic() + timeout_s
    while True:
        job = studio_call("GET", f"/api/jobs/{job_id}")
        if job.get("state") in ("ok", "failed", "cancelled"):
            return job
        if time.monotonic() >= deadline:
            return {**job, "state": job.get("state", "?") + " (timed out waiting)"}
        time.sleep(3)


class StudioJobArgs(BaseModel):
    kind: str = Field(description="one of: build, check, validate, host-shot")
    target: str = Field(description="package name, check name, or validator id")
    path: str = Field(default="", description="for validate: the path the validator runs on")
    wait: bool = Field(default=True, description="wait for the job and return its report")


class StudioJob(BaseTool):
    name: str = "studio_job"
    description: str = ("Run a build, check, validator or host-shot THROUGH Kiln Studio "
                        "(shared queue, correct argv, visible in the Jobs panel). Returns the "
                        "job's state, exit code and JSON report.")
    args_schema: type[BaseModel] = StudioJobArgs
    timeout_s: int = 900

    def _run(self, kind: str, target: str, path: str = "", wait: bool = True) -> str:
        if kind not in KINDS:
            return f"studio job kind {kind!r} refused: one of {', '.join(KINDS)}"
        body: dict = {"kind": kind, "target": target}
        if kind == "validate":
            if not path:
                return "validate needs a path (e.g. assets/map_demo.map)"
            body["arg"] = path
        job = studio_call("POST", "/api/jobs", body)
        if not wait:
            return json.dumps({"id": job.get("id"), "submitted": True})


        out = wait_job(job["id"], self.timeout_s)
        keep = {k: out[k] for k in ("id", "kind", "target", "state", "exit") if k in out}
        if out.get("report") is not None:
            keep["report"] = out["report"]
        if out.get("shot"):
            base, _ = _studio_base_and_token()
            keep["shot"] = f"{base}/api/jobs/{out['id']}/shot.png"
        return json.dumps(keep)
