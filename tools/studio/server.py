#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Kiln Studio — one local web app for working on Kiln games.

    nix run .#studio            (or ./dev studio)
    server.py --repo DIR [--host 127.0.0.1] [--port 8420] [--allow-host NAME]...
              [--tailscale-login LOGIN]... [--trusted-proxy ADDR]... [--max-jobs N]
              [--manifest-file F] [--caps-file F] [--nix PATH]

It serves the studio page and a small JSON API over the repository's real tools:
the project manifest the flake generates, builds and checks run as jobs with
streamed logs, the machine's capabilities, and the map maker and poser with
compare-and-swap saves, advisory locks and presence for a shared session. It
reimplements none of them.

Binds 127.0.0.1 by default. For a shared session over a tailnet, run
`tailscale serve` in front of it and pass --tailscale-login for each person; the
identity headers the proxy adds are trusted only from --trusted-proxy (loopback).

KILN_STUDIO_BREAK=<guard>[,<guard>] switches a protection OFF. It exists for
tests/api_test.py, which proves each guard is what stops its attack; the server
says so loudly on stderr when any is set.
"""

import argparse
import base64
import hashlib
import json
import mimetypes
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from studio.agents import AgentTasks  # noqa: E402
from studio.auth import Auth, load_or_create_token  # noqa: E402
from studio.fsapi import Files, FsError  # noqa: E402
from studio.jobs import JobError, Runner  # noqa: E402
from studio.presence import Presence  # noqa: E402
from studio.project import Project  # noqa: E402
from studio.validators import registry as validator_registry  # noqa: E402

GUARDS = ("host", "origin", "token", "identity-source", "argv", "traversal", "cancel-group",
          "fs-allow", "fs-escape", "stale-write", "lock")
MAX_BODY = 3 << 20
SECURITY_HEADERS = {
    "Content-Security-Policy": "default-src 'self'; img-src 'self' data: blob:; connect-src 'self'; "
                               "frame-src 'self'; object-src 'none'; base-uri 'none'; form-action 'self'; "
                               "frame-ancestors 'self'",
    "X-Content-Type-Options": "nosniff",
    "Referrer-Policy": "no-referrer",
    "X-Frame-Options": "SAMEORIGIN",
}

# What the server hands out without an API call. The studio's own page and
# modules; the editors' code, from the tools/ directory this server lives in;
# and, signed in only, the poser's staged data from the repository being edited.
STUDIO_STATIC = ("index.html", "src/")
EDITOR_STATIC = ("tools/mapmaker/index.html", "tools/mapmaker/src/", "tools/mapmaker/vendor/",
                 "tools/poser/index.html", "tools/poser/src/", "tools/webcommon/")
REPO_STATIC = ("tools/poser/data/",)


def _under(rel, prefixes):
    return any(rel == p or (p.endswith("/") and rel.startswith(p)) for p in prefixes)


def editor_csp(html, wasm=False):
    """The editors' pages carry an inline importmap and inline styles. Rather
    than allow inline script, allow exactly the inline scripts this page has, by
    hash; anything injected later matches no hash and does not run. A game page
    also needs 'wasm-unsafe-eval', which permits compiling WebAssembly and
    nothing else — not eval, not new Function."""
    hashes = ("'wasm-unsafe-eval' " if wasm else "") + " ".join("'sha256-" + base64.b64encode(hashlib.sha256(m.group(1)).digest()).decode() + "'"
                      for m in re.finditer(rb"<script\b(?![^>]*\bsrc=)[^>]*>(.*?)</script>", html, re.S))
    return ("default-src 'self'; script-src 'self' " + hashes + "; style-src 'self' 'unsafe-inline'; "
            "img-src 'self' data: blob:; connect-src 'self'; object-src 'none'; base-uri 'none'; "
            "form-action 'self'; frame-ancestors 'self'")


class Studio:
    def __init__(self, args, breaks):
        self.repo = Path(args.repo).resolve()
        self.state = self.repo / ".studio"
        self.breaks = breaks
        # Human token may live outside the checkout (KILN_STUDIO_TOKEN_FILE)
        # so a container that mounts the project cannot present as the human.
        token = load_or_create_token(Path(os.environ.get("KILN_STUDIO_TOKEN_FILE", self.state / "token")))
        agents_token = load_or_create_token(self.state / "agents-token")
        self.auth = Auth(token, args.allow_host, args.tailscale_login,
                         args.trusted_proxy or ("127.0.0.1", "::1"), breaks,
                         agents_token=agents_token)
        self.project = Project(self.repo, args.nix, args.manifest_file, args.caps_file)
        self.project.load()
        self.validators = validator_registry(self.repo, sys.executable, args.nix)
        self.files = Files(self.repo, HERE / "allow.json", breaks)
        self.presence = Presence()
        self.runner = Runner(self.repo, args.nix, lambda: self.project.manifest, self.state,
                             args.max_jobs, breaks, self.validators)
        self.agent_tasks = AgentTasks(self.repo)
        self.static_root = HERE


class Handler(BaseHTTPRequestHandler):
    server_version = "KilnStudio/1"
    protocol_version = "HTTP/1.1"

    @property
    def studio(self):
        return self.server.studio

    def log_message(self, fmt, *args):
        if os.environ.get("KILN_STUDIO_VERBOSE"):
            sys.stderr.write("studio: " + (fmt % args) + "\n")

    # ── responses ─────────────────────────────────────────────────────────
    def send(self, code, body=b"", ctype="application/json; charset=utf-8", headers=None):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        for k, v in {**SECURITY_HEADERS, **(headers or {})}.items():
            self.send_header(k, v)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def json(self, code, obj, headers=None):
        hdrs = {"Cache-Control": "no-store"}
        hdrs.update(headers or {})
        self.send(code, json.dumps(obj), headers=hdrs)

    def error(self, code, msg):
        self.json(code, {"error": msg})

    def body_json(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n > MAX_BODY:
            raise ValueError("request body too large")
        raw = self.rfile.read(n) if n else b""
        return json.loads(raw or b"{}")

    # ── routing ───────────────────────────────────────────────────────────
    def do_GET(self):
        self.route("GET")

    def do_HEAD(self):
        self.route("GET")

    def do_POST(self):
        self.route("POST")

    def do_PATCH(self):
        self.route("PATCH")

    def route(self, method):
        auth = self.studio.auth
        if not auth.host_ok(self.headers.get("Host")):
            return self.error(403, "this server does not answer to that Host")
        if not auth.origin_ok(method, self.headers.get("Origin"), self.headers.get("Host")):
            return self.error(403, "cross-origin request refused")

        path = urlparse(self.path).path
        if not path.startswith("/api/"):
            return self.static(path) if method == "GET" else self.error(405, "method not allowed")
        query = parse_qs(urlparse(self.path).query)
        if path == "/api/session" and method == "POST":
            return self.session()

        who = auth.identify(self.client_address[0], self.headers)
        if who is None:
            return self.error(401, "not signed in")
        self.user, self.via = who

        parts = [p for p in path.split("/") if p][1:]      # drop "api"
        try:
            if method == "GET" and parts == ["whoami"]:
                return self.json(200, {"user": self.user, "via": self.via})
            if method == "GET" and parts == ["manifest"]:
                return self.manifest()
            if method == "POST" and parts == ["manifest", "reload"]:
                self.studio.project.reload_manifest()
                return self.json(202, {"reloading": True})
            if method == "GET" and parts == ["caps"]:
                return self.caps()
            if method == "GET" and parts == ["validators"]:
                return self.json(200, {"validators": [
                    {"id": vid, "label": spec["label"], "args": spec["args"]()}
                    for vid, spec in self.studio.validators.items()]})
            if method == "GET" and parts == ["jobs"]:
                return self.json(200, {"jobs": self.studio.runner.list()})
            if parts[:1] == ["fs"]:
                return self.fs(method, parts[1:], query)
            if method == "GET" and parts == ["presence"]:
                return self.json(200, {"people": self.studio.presence.list()})
            if method == "POST" and parts == ["presence"]:
                body = self.body_json()
                self.studio.presence.beat(self.user, body.get("client"), body.get("panel"), body.get("file"))
                return self.json(200, {"people": self.studio.presence.list()})
            if method == "POST" and parts == ["jobs"]:
                return self.submit()
            if parts[:1] == ["agent-tasks"]:
                return self.agent_tasks_endpoint(method, parts[1:])
            if len(parts) >= 2 and parts[0] == "jobs":
                job = self.studio.runner.get(parts[1])
                if job is None:
                    return self.error(404, "no such job")
                if method == "GET" and len(parts) == 2:
                    return self.json(200, job.snapshot())
                if method == "GET" and parts[2:] == ["events"]:
                    return self.events(job)
                if method == "POST" and parts[2:] == ["cancel"]:
                    return self.json(200, {"cancelled": self.studio.runner.cancel(job)})
                if method == "GET" and parts[2:] == ["shot.png"]:
                    if not (job.shot and Path(job.shot).is_file()):
                        return self.error(404, "no frame from that job")
                    return self.send(200, Path(job.shot).read_bytes(), ctype="image/png",
                                     headers={"Cache-Control": "no-store"})
            if method == "GET" and len(parts) == 2 and parts[0] == "play":
                return self.json(200, {"package": parts[1],
                                       "ready": self.studio.runner.latest_output(parts[1]) is not None})
            return self.error(404, "no such endpoint")
        except FsError as e:
            return self.json(e.status, {"error": str(e), **e.extra})
        except (ValueError, JobError) as e:
            return self.error(400, str(e))

    # ── endpoints ─────────────────────────────────────────────────────────
    def session(self):
        try:
            token = str(self.body_json().get("token", ""))
        except ValueError:
            return self.error(400, "bad request body")
        if not self.studio.auth.token_matches(token):
            return self.error(401, "wrong token")
        self.send(204, headers={"Set-Cookie": self.studio.auth.session_cookie(), "Cache-Control": "no-store"})

    def manifest(self):
        p = self.studio.project
        if p.manifest is None:
            return self.json(503, {"error": "manifest loading", "detail": p.errors.get("manifest")})
        return self.json(200, p.manifest)

    def caps(self):
        p = self.studio.project
        caps = dict(p.caps or {})
        # Actions that open a window or touch hardware on THIS machine are for
        # someone sitting at it; a remote collaborator gets them disabled.
        caps["local_client"] = self.client_address[0] in ("127.0.0.1", "::1") and self.via == "token"
        caps["loading"] = p.caps is None
        caps["error"] = p.errors.get("caps")
        return self.json(200, caps)

    def submit(self):
        body = self.body_json()
        arg = body.get("arg")
        job = self.studio.runner.submit(str(body.get("kind", "")), body.get("target", ""), self.user,
                                        arg=arg if isinstance(arg, str) else None)
        return self.json(201, job.snapshot())

    def agent_tasks_endpoint(self, method, parts):
        """/api/agent-tasks: humans create and decide; the flow PATCHes its
        own record as it runs. See studio/agents.py for the lifecycle."""
        tasks = self.studio.agent_tasks
        if method == "GET" and not parts:
            return self.json(200, {"tasks": tasks.list(),
                                   "agents_service": self.studio.agent_tasks.url or None})
        if method == "POST" and not parts:
            if self.user == "agents":
                return self.error(403, "the agents identity cannot create tasks")
            body = self.body_json()
            return self.json(201, tasks.create(str(body.get("brief", "")), self.user))
        if not parts:
            return self.error(404, "no such endpoint")
        try:
            if method == "GET" and len(parts) == 1:
                rec = tasks.get(parts[0])
                return self.json(200, rec) if rec else self.error(404, "no such task")
            if method == "PATCH" and len(parts) == 1:
                return self.json(200, tasks.patch(parts[0], self.body_json()))
            if method == "POST" and len(parts) == 2 and parts[1] in ("approve", "reject", "merge", "discard"):
                if self.user == "agents":
                    return self.error(403, "the agents identity cannot decide or merge")
                if parts[1] in ("approve", "reject"):
                    return self.json(200, tasks.decide(parts[0], parts[1] == "approve", self.user))
                if parts[1] == "merge":
                    return self.json(200, tasks.merge(parts[0]))
                return self.json(200, tasks.discard(parts[0]))
        except KeyError:
            return self.error(404, "no such task")
        return self.error(404, "no such endpoint")

    def fs(self, method, parts, query):
        files = self.studio.files
        if method == "GET" and parts == ["list"]:
            return self.json(200, {"files": files.list()})
        if method == "GET" and parts == ["read"]:
            return self.json(200, files.read((query.get("path") or [None])[0]))
        if method != "POST" or parts not in (["write"], ["lock"], ["unlock"]):
            return self.error(404, "no such endpoint")
        body = self.body_json()
        rel, take = body.get("path"), body.get("take") is True
        if parts == ["lock"]:
            return self.json(200, {"lock": files.lock(rel, self.user, take=take)})
        if parts == ["unlock"]:
            return self.json(200, {"released": files.unlock(rel, self.user)})
        result = files.write(rel, body.get("text"), body.get("baseSha256"), self.user, take=take)
        # A save is followed by the file's validator, so whoever saved — and
        # whoever else is watching the jobs — sees at once whether it still holds.
        spec = self.studio.validators.get(result.get("validator") or "")
        if spec and rel in spec["args"]():
            try:
                result["job"] = self.studio.runner.submit("validate", result["validator"], self.user, arg=rel).id
            except JobError as e:
                result["validate_error"] = str(e)
        return self.json(200, result)

    def events(self, job):
        try:
            last = int(self.headers.get("Last-Event-ID") or 0)
        except ValueError:
            last = 0
        self.send_response(200)
        for k, v in SECURITY_HEADERS.items():
            self.send_header(k, v)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        try:
            while True:
                with job.cond:
                    if job.seq <= last and not job.finished:
                        job.cond.wait(timeout=15)
                    new = job.since(last)
                    done = job.finished and (not new or new[-1][0] >= job.seq)
                if not new and not done:
                    self.wfile.write(b": keepalive\n\n")
                for seq, text in new:
                    self.wfile.write(f"id: {seq}\nevent: line\ndata: {json.dumps(text)}\n\n".encode())
                    last = seq
                if done:
                    self.wfile.write(f"event: done\ndata: {json.dumps(job.snapshot())}\n\n".encode())
                    self.wfile.flush()
                    return
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            return

    def play(self, rel):
        """A browser build this session built, served from its own output:
        /play/<web package>/index.html is the build's page, anything else a
        plain file beside it in bin/."""
        studio = self.studio
        if studio.auth.identify(self.client_address[0], self.headers) is None:
            return self.error(401, "not signed in")
        parts = rel.split("/")
        pkg, name = parts[1] if len(parts) > 1 else "", "/".join(parts[2:]) or "index.html"
        if ((studio.project.manifest or {}).get("packages", {}).get(pkg) or {}).get("kind") != "web":
            return self.error(404, "not a browser build")
        out = studio.runner.latest_output(pkg)
        if out is None:
            return self.error(404, "not built in this session — build it first")
        bindir = Path(out) / "bin"
        if name == "index.html":
            pages = sorted(bindir.glob("*.html")) if bindir.is_dir() else []
            if len(pages) != 1:
                return self.error(404, "that build has no page")
            target = pages[0]
        else:
            if "traversal" not in studio.breaks and ("/" in name or name.startswith(".")):
                return self.error(404, "not found")
            target = (bindir / name).resolve()
            if "traversal" not in studio.breaks and target.parent != bindir.resolve():
                return self.error(404, "not found")
        if not target.is_file():
            return self.error(404, "not found")
        body = target.read_bytes()
        ctype = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
                 ".wasm": "application/wasm"}.get(target.suffix, "application/octet-stream")
        headers = {"Cache-Control": "no-cache"}
        if target.suffix == ".html":
            headers["Content-Security-Policy"] = editor_csp(body, wasm=True)
        self.send(200, body, ctype=ctype, headers=headers)

    def static(self, path):
        studio = self.studio
        rel = unquote(path).lstrip("/") or "index.html"
        if rel.startswith("play/"):
            return self.play(rel)
        if rel in ("tools/mapmaker/", "tools/poser/"):
            rel += "index.html"
        if _under(rel, REPO_STATIC):
            root = studio.repo
            if studio.auth.identify(self.client_address[0], self.headers) is None:
                return self.error(401, "not signed in")
        elif _under(rel, EDITOR_STATIC):
            root = HERE.parent.parent
        else:
            root = studio.static_root
        if "traversal" not in studio.breaks:
            if ".." in rel.split("/") or not _under(rel, STUDIO_STATIC + EDITOR_STATIC + REPO_STATIC):
                return self.error(404, "not found")
        target = (root / rel).resolve()
        if "traversal" not in studio.breaks and root.resolve() not in target.parents:
            return self.error(404, "not found")
        if not target.is_file():
            return self.error(404, "not found")
        ctype = mimetypes.guess_type(target.name)[0] or "application/octet-stream"
        if target.suffix in (".js", ".mjs"):
            ctype = "text/javascript"
        body = target.read_bytes()
        headers = {"Cache-Control": "no-cache"}
        if rel.startswith("tools/") and target.suffix == ".html":
            headers["Content-Security-Policy"] = editor_csp(body)
        self.send(200, body, ctype=ctype + ("; charset=utf-8" if ctype.startswith("text") else ""), headers=headers)


def main(argv):
    ap = argparse.ArgumentParser(prog="kiln-studio")
    ap.add_argument("--repo", default=os.environ.get("KILN_REPO", "."))
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8420)
    ap.add_argument("--allow-host", action="append", default=[])
    ap.add_argument("--tailscale-login", action="append", default=[])
    ap.add_argument("--trusted-proxy", action="append", default=[])
    ap.add_argument("--max-jobs", type=int, default=2)
    ap.add_argument("--manifest-file")
    ap.add_argument("--caps-file")
    ap.add_argument("--nix", default="nix")
    args = ap.parse_args(argv)

    breaks = tuple(b for b in os.environ.get("KILN_STUDIO_BREAK", "").split(",") if b)
    unknown = set(breaks) - set(GUARDS)
    if unknown:
        ap.error(f"KILN_STUDIO_BREAK names unknown guards {sorted(unknown)}; have {GUARDS}")
    if breaks:
        sys.stderr.write(f"studio: WARNING — protections disabled for testing: {', '.join(breaks)}\n")

    studio = Studio(args, breaks)
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True
    server.studio = studio
    host, port = server.server_address[:2]
    print(f"listening {host} {port}", flush=True)
    shown = "localhost" if host in ("127.0.0.1", "::1") else host
    print(f"kiln studio: http://{shown}:{port}/#token={studio.auth.token}", flush=True)
    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
