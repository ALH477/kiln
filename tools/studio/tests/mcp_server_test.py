#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""mcp_server_test.py — the studio's MCP server against a stub studio.

Runs entirely in-process: a stub answers the handful of studio API endpoints
tools/studio/mcp_server.py calls (manifest, job submit/poll, the SSE log
replay), and a real MCP stdio client drives the server. Proves the tool layer
without a studio, a browser or the network: tools are listed, a build job is
submitted with the studio's exact contract (kind+target as JSON, bearer token),
a settled job's report comes back, and the log replay is read to its `done`
event. Also proves the stub's 401 (wrong token) surfaces as a tool error rather
than a hang.

Run by nix/checks/agent-env.nix; exits non-zero on any failure.
"""

import asyncio
import importlib.util
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SPEC = importlib.util.spec_from_file_location(
    "mcp_server", __file__.replace("/tests/mcp_server_test.py", "/mcp_server.py"))
mcp_server = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mcp_server)

TOKEN = "t0ken"
JOBS = {}


class Stub(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _auth(self):
        if self.headers.get("Authorization") != f"Bearer {TOKEN}":
            self.send_response(401)
            self.end_headers()
            self.wfile.write(b'{"error": "not signed in"}')
            return False
        return True

    def _json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if not self._auth():
            return
        if self.path == "/api/manifest":
            return self._json({"games": {"demo": {"packages": ["demo", "pc-demo"]}}, "checks": ["kiln-gui"]})
        if self.path == "/api/jobs/j1":
            return self._json({"id": "j1", "kind": "build", "target": "pc-demo",
                               "state": "ok", "exit": 0, "report": {"tool": "studio", "ok": True}})
        if self.path == "/api/jobs/j1/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            for i, line in enumerate(("building pc-demo", "ok"), 1):
                self.wfile.write(f"id: {i}\nevent: line\ndata: {json.dumps(line)}\n\n".encode())
            self.wfile.write(b'event: done\ndata: {"state": "ok"}\n\n')
            return
        self.send_response(404)
        self.end_headers()

    def do_POST(self):
        if not self._auth():
            return
        if self.path == "/api/jobs":
            n = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(n) or b"{}")
            assert body == {"kind": "build", "target": "pc-demo"}, body
            return self._json({"id": "j1"}, 201)
        self.send_response(404)
        self.end_headers()


async def main():
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Stub)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{srv.server_address[1]}"

    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client
    script = SPEC.origin
    params = StdioServerParameters(
        command=__import__("sys").executable,
        args=[script, "--stdio", "--studio", base],
        env={"PATH": "/usr/bin:/bin", "KILN_STUDIO_TOKEN": TOKEN})
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            tools = {t.name for t in (await session.list_tools()).tools}
            want = {"studio_list_games", "studio_list_validators", "studio_build", "studio_run_check",
                    "studio_validate", "studio_host_shot", "studio_job", "studio_job_log"}
            assert want <= tools, f"missing tools: {want - tools}"
            print("  ok   tools listed")

            r = await session.call_tool("studio_list_games", {})
            games = json.loads(r.content[0].text)["games"]
            assert "demo" in games, games
            print("  ok   studio_list_games")

            r = await session.call_tool("studio_build", {"package": "pc-demo"})
            rep = json.loads(r.content[0].text)
            assert rep["state"] == "ok" and rep["report"]["ok"] is True, rep
            print("  ok   studio_build waited and returned the report")

            r = await session.call_tool("studio_job_log", {"job_id": "j1"})
            log = json.loads(r.content[0].text)
            assert log["state"] == "ok" and "building pc-demo" in log["log"], log
            print("  ok   studio_job_log read the replay to done")

    # Wrong token: the server must surface the studio's refusal as a tool error.
    params.env = dict(params.env, KILN_STUDIO_TOKEN="wrong")
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            r = await session.call_tool("studio_list_games", {})
            assert r.isError and "401" in r.content[0].text, r.content
            print("  ok   a rejected token surfaces as 401")

    srv.shutdown()
    print("mcp_server test: ok")


asyncio.run(main())
