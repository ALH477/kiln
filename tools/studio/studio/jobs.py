# SPDX-License-Identifier: MIT
"""Long-running work — nix builds and checks — as jobs anyone in the session can watch.

A job is one subprocess with a fixed argv built HERE from a job type and a target
the manifest names. Nothing a client sends reaches a shell, and a target that is
not a package or check the flake itself declared is refused before any process
starts (the "argv" guard).

Each job runs in its own process group, so cancelling kills the whole build tree
nix spawned and not just the `nix` process (the "cancel-group" guard: without it
the builders outlive the cancel and keep burning the shared machine).

Logs are numbered lines. The SSE stream sends each with its number as the event
id, so a browser that drops and reconnects resumes from Last-Event-ID instead of
replaying or skipping.
"""

import json
import os
import re
import signal
import subprocess
import threading
import time
import uuid
from pathlib import Path

TARGET_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]{0,99}$")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
MAX_LINES = 100_000
KINDS = ("build", "check", "cheap")


class JobError(ValueError):
    pass


class Job:
    def __init__(self, kind, target, argv, user):
        self.id = uuid.uuid4().hex[:12]
        self.kind, self.target, self.argv, self.user = kind, target, argv, user
        self.state = "queued"
        self.created = time.time()
        self.started = self.ended = None
        self.exit = None
        self.outputs = []
        self.lines = []          # (seq, text); seq starts at 1
        self.seq = 0
        self.cond = threading.Condition()
        self.proc = None
        self.log_file = None

    @property
    def finished(self):
        return self.state in ("ok", "failed", "cancelled")

    def add(self, text):
        with self.cond:
            self.seq += 1
            if len(self.lines) < MAX_LINES:
                self.lines.append((self.seq, text))
            if self.log_file:
                self.log_file.write(text + "\n")
                self.log_file.flush()
            self.cond.notify_all()

    def set_state(self, state):
        with self.cond:
            self.state = state
            if state == "running":
                self.started = time.time()
            if state in ("ok", "failed", "cancelled"):
                self.ended = time.time()
            self.cond.notify_all()

    def since(self, last):
        return [(s, t) for s, t in self.lines if s > last]

    def snapshot(self):
        return {
            "id": self.id, "kind": self.kind, "target": self.target, "user": self.user,
            "state": self.state, "exit": self.exit, "outputs": list(self.outputs),
            "created": self.created, "started": self.started, "ended": self.ended,
            "lines": self.seq, "argv": list(self.argv),
        }


def parse_nix_line(line):
    """One stderr line from `nix --log-format internal-json`, as display text or None."""
    line = line.rstrip("\n")
    if not line.startswith("@nix "):
        return ANSI_RE.sub("", line)
    try:
        ev = json.loads(line[5:])
    except ValueError:
        return line
    action = ev.get("action")
    if action == "msg":
        return ANSI_RE.sub("", ev.get("msg", ""))
    if action == "start" and ev.get("text"):
        return "▸ " + ANSI_RE.sub("", ev["text"])
    if action == "result" and ev.get("type") == 101 and ev.get("fields"):
        return ANSI_RE.sub("", str(ev["fields"][0]))
    return None


class Runner:
    def __init__(self, repo, nix, manifest, state_dir, max_jobs=2, breaks=()):
        self.repo = Path(repo)
        self.nix = nix
        self.manifest = manifest       # callable -> manifest dict
        self.state_dir = Path(state_dir) / "jobs"
        self.state_dir.mkdir(parents=True, exist_ok=True)
        self.sem = threading.Semaphore(max_jobs)
        self.breaks = set(breaks)
        self.jobs = {}
        self.lock = threading.Lock()

    # ── argv, from a fixed menu ───────────────────────────────────────────
    def argv(self, kind, target):
        m = self.manifest()
        if m is None:
            raise JobError("the project manifest is not loaded yet")
        flake = str(self.repo)
        base = [self.nix, "build", "--no-link", "--print-out-paths", "--log-format", "internal-json", "-L"]
        if "argv" not in self.breaks:
            if kind not in KINDS:
                raise JobError(f"unknown job type {kind!r}")
            if kind != "cheap" and not (isinstance(target, str) and TARGET_RE.match(target)):
                raise JobError(f"invalid target {target!r}")
        if kind == "build":
            if "argv" not in self.breaks and target not in m.get("packages", {}):
                raise JobError(f"{target!r} is not a package in the manifest")
            return base + [f"{flake}#{target}"]
        if kind == "check":
            if "argv" not in self.breaks and target not in m.get("checks", []):
                raise JobError(f"{target!r} is not a check in the manifest")
            return base + [f"{flake}#checks.{m['system']}.{target}"]
        if kind == "cheap":
            return base + [f"{flake}#checks.{m['system']}.{c}" for c in m.get("cheap", [])]
        if "argv" in self.breaks:
            return base + [f"{flake}#{target}"]
        raise JobError(f"unknown job type {kind!r}")

    # ── lifecycle ─────────────────────────────────────────────────────────
    def submit(self, kind, target, user):
        job = Job(kind, target, self.argv(kind, target), user)
        with self.lock:
            self.jobs[job.id] = job
        threading.Thread(target=self._run, args=(job,), daemon=True).start()
        return job

    def get(self, jid):
        with self.lock:
            return self.jobs.get(jid)

    def list(self, limit=100):
        with self.lock:
            jobs = sorted(self.jobs.values(), key=lambda j: j.created, reverse=True)
        return [j.snapshot() for j in jobs[:limit]]

    def _run(self, job):
        job.log_file = open(self.state_dir / f"{job.id}.log", "w")
        job.add(f"$ {' '.join(job.argv)}")
        with self.sem:
            if job.state == "cancelled":
                return self._finish(job)
            job.set_state("running")
            try:
                job.proc = subprocess.Popen(
                    job.argv, cwd=self.repo, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    text=True, bufsize=1, start_new_session="cancel-group" not in self.breaks)
            except OSError as e:
                job.add(f"could not start: {e}")
                job.exit = 127
                job.set_state("failed")
                return self._finish(job)

            def pump_out():
                for line in job.proc.stdout:
                    line = line.strip()
                    if line:
                        job.outputs.append(line)
                        job.add("→ " + line)

            def pump_err():
                for line in job.proc.stderr:
                    text = parse_nix_line(line)
                    if text:
                        job.add(text)

            readers = [threading.Thread(target=pump_out, daemon=True),
                       threading.Thread(target=pump_err, daemon=True)]
            for r in readers:
                r.start()
            job.exit = job.proc.wait()
            for r in readers:
                r.join(timeout=5)
            if job.state != "cancelled":
                job.set_state("ok" if job.exit == 0 else "failed")
        self._finish(job)

    def _finish(self, job):
        job.add(f"── {job.state} (exit {job.exit})")
        (self.state_dir / f"{job.id}.json").write_text(json.dumps(job.snapshot(), indent=1))
        if job.log_file:
            job.log_file.close()
            job.log_file = None

    def cancel(self, job):
        if job.finished:
            return False
        job.set_state("cancelled")
        proc = job.proc
        if proc is None or proc.poll() is not None:
            return True
        try:
            if "cancel-group" in self.breaks:
                proc.terminate()
            else:
                os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        except ProcessLookupError:
            return True

        def escalate():
            time.sleep(5)
            if proc.poll() is None:
                try:
                    if "cancel-group" in self.breaks:
                        proc.kill()
                    else:
                        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                except ProcessLookupError:
                    pass
        threading.Thread(target=escalate, daemon=True).start()
        return True
