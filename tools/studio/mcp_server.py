#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""mcp_server.py — Kiln Studio's API as MCP tools, for agents.

The studio is the single authority on builds, checks and validators: its job
kinds build their own argv, its semaphore is the build budget, its job log is
where both people and agents watch work happen. This server exposes that same
surface to MCP clients — CrewAI role agents and Claude Code workers — rather
than letting an agent talk to nix directly, because a job the studio ran is a
job anyone in the session can see, cancel, and re-run.

It is a thin client of the studio's own HTTP API: no logic is duplicated here
beyond shaping results. Every tool authenticates with the studio's token, and
everything the studio refuses (bad package, unknown check, an arg outside a
validator's allowlist) is refused by the studio, not re-decided here.

    mcp_server.py --studio http://127.0.0.1:8420 [--token-file F] [--port 8500]

Transport: streamable HTTP, bound to the compose network by the module — the
studio container serves it beside the studio itself. For a local smoke run it
also speaks stdio: `mcp_server.py --stdio`.
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

MAX_LOG = 4000  # characters of a job log tail a tool returns


class Studio:
    def __init__(self, base, token):
        self.base = base.rstrip("/")
        self.token = token

    def call(self, method, path, body=None):
        # The studio refuses an origin-less POST (CSRF guard); a bearer-token
        # client presents the studio's own origin, the same shape a same-origin
        # browser tab sends.
        from urllib.parse import urlsplit
        origin = urlsplit(self.base)
        req = urllib.request.Request(
            self.base + path, method=method,
            data=json.dumps(body).encode() if body is not None else None,
            headers={"Authorization": f"Bearer {self.token}",
                     "Content-Type": "application/json",
                     "Origin": f"{origin.scheme}://{origin.netloc}"})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                raw = r.read()
                return json.loads(raw) if raw.strip() else {}
        except urllib.error.HTTPError as e:
            detail = e.read().decode(errors="replace")[:300]
            raise ValueError(f"studio answered {e.code} for {path}: {detail}")

    def job_report(self, job_id, wait_s=0):
        """A job snapshot; with wait_s > 0, poll until it settles or time runs out."""
        import time
        deadline = time.monotonic() + max(0, min(wait_s, 3600))
        while True:
            job = self.call("GET", f"/api/jobs/{job_id}")
            if job["state"] in ("ok", "failed", "cancelled") or time.monotonic() >= deadline:
                return job
            time.sleep(2)

    def job_summary(self, job):
        out = {k: job[k] for k in ("id", "kind", "target", "state", "exit") if k in job}
        if job.get("report") is not None:
            out["report"] = job["report"]
        if job.get("shot"):
            out["shot"] = f"{self.base}/api/jobs/{job['id']}/shot.png"
        return out

    def job_log(self, job_id, wait=False, tail=MAX_LOG):
        """A job's log as text. The events endpoint replays a settled job's whole
        log and closes; for one still running, wait=True follows it to the end."""
        req = urllib.request.Request(f"{self.base}/api/jobs/{job_id}/events",
                                     headers={"Authorization": f"Bearer {self.token}"})
        lines = []
        with urllib.request.urlopen(req, timeout=3600 if wait else 10) as r:
            state = "unknown"
            try:
                for raw in r:
                    line = raw.decode(errors="replace").rstrip("\n")
                    if line.startswith("data: "):
                        data = json.loads(line[6:])
                        if isinstance(data, str):
                            lines.append(data)
                        elif isinstance(data, dict):      # the `done` event's snapshot
                            state = data.get("state", state)
                            break
            except (TimeoutError, urllib.error.URLError):
                state = "running (log truncated; wait=True follows it to the end)"
        text = "\n".join(lines)
        if len(text) > tail:
            text = f"[first {len(text) - tail} characters dropped]\n" + text[-tail:]
        return {"id": job_id, "state": state, "log": text}


def build_server(studio):
    from mcp.server.fastmcp import FastMCP

    mcp = FastMCP("kiln-studio", instructions=(
        "Kiln Studio's build and validation surface. studio_list_games shows what exists; "
        "studio_build / studio_run_check / studio_validate submit jobs (the studio builds the argv — "
        "an agent never can); studio_job waits on one and returns its report. Package names come from "
        "the manifest, validator ids and their arguments from studio_list_validators."))

    def dump(obj):
        return json.dumps(obj, ensure_ascii=False)

    @mcp.tool()
    def studio_list_games() -> str:
        """Every game and its packages (ROM, jump ROMs, pc/web builds), plus the check list."""
        m = studio.call("GET", "/api/manifest")
        return dump({"games": m.get("games"), "checks": m.get("checks")})

    @mcp.tool()
    def studio_list_validators() -> str:
        """The validators (map-validate, camlint, ...) and the paths each one can run on."""
        return dump(studio.call("GET", "/api/validators"))

    @mcp.tool()
    def studio_build(package: str, wait: bool = True) -> str:
        """Build a package from the manifest (a ROM or a pc-/web- host build) and — by default —
        wait for it, returning the report. The package must exist in the manifest."""
        job = studio.call("POST", "/api/jobs", {"kind": "build", "target": package})
        return dump(studio.job_summary(studio.job_report(job["id"], 3600 if wait else 0)))

    @mcp.tool()
    def studio_run_check(check: str, wait: bool = True) -> str:
        """Run one of the flake's checks by name (studio_list_games lists them) and return its result."""
        job = studio.call("POST", "/api/jobs", {"kind": "check", "target": check})
        return dump(studio.job_summary(studio.job_report(job["id"], 3600 if wait else 0)))

    @mcp.tool()
    def studio_validate(validator: str, path: str, wait: bool = True) -> str:
        """Run a validator over a repo path it advertises (map-validate over assets/*.map,
        camlint over a *.shot.json, ...). Returns the JSON report."""
        job = studio.call("POST", "/api/jobs", {"kind": "validate", "target": validator, "arg": path})
        return dump(studio.job_summary(studio.job_report(job["id"], 600 if wait else 0)))

    @mcp.tool()
    def studio_host_shot(package: str, wait: bool = True) -> str:
        """Render 120 frames of a pc- package with the real renderer and keep the PNG
        (the report carries its URL). No emulator, no compositor."""
        job = studio.call("POST", "/api/jobs", {"kind": "host-shot", "target": package})
        return dump(studio.job_summary(studio.job_report(job["id"], 600 if wait else 0)))

    @mcp.tool()
    def studio_map_render(path: str, wait: bool = True) -> str:
        """Draw a map under assets/ with the real engine and keep the PNG (the result
        carries its shot URL) — the way an agent SEES a level it just authored."""
        job = studio.call("POST", "/api/jobs", {"kind": "map-render", "target": path})
        return dump(studio.job_summary(studio.job_report(job["id"], 600 if wait else 0)))

    @mcp.tool()
    def studio_job(job_id: str, wait_s: float = 0) -> str:
        """A job's state, report and shot URL; wait_s polls until it settles (or time runs out)."""
        return dump(studio.job_summary(studio.job_report(job_id, wait_s)))

    @mcp.tool()
    def studio_job_log(job_id: str, wait: bool = False) -> str:
        """A job's full log (as text, tailed to a few thousand characters). wait=True follows a
        running job to the end; otherwise a running job's log is returned truncated so far."""
        return dump(studio.job_log(job_id, wait=wait))

    return mcp


def main(argv):
    ap = argparse.ArgumentParser(prog="mcp_server.py", description=__doc__.split("\n\n")[0])
    ap.add_argument("--studio", default=os.environ.get("KILN_STUDIO_URL", "http://127.0.0.1:8420"))
    ap.add_argument("--token-file", default=None,
                    help="the studio's session token (default: $KILN_REPO/.studio/token)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8500)
    ap.add_argument("--stdio", action="store_true", help="serve stdio instead of HTTP (local smoke)")
    args = ap.parse_args(argv)

    repo = Path(os.environ.get("KILN_REPO", os.getcwd()))
    token_file = Path(args.token_file) if args.token_file else repo / ".studio" / "token"
    token = os.environ.get("KILN_STUDIO_TOKEN") or (token_file.read_text().strip() if token_file.is_file() else "")
    if not token:
        print(f"mcp_server: no studio token (looked in $KILN_STUDIO_TOKEN and {token_file})", file=sys.stderr)
        return 2

    mcp = build_server(Studio(args.studio, token))
    if args.stdio:
        mcp.run("stdio")
    else:
        mcp.settings.host, mcp.settings.port = args.host, args.port
        mcp.run("streamable-http")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
