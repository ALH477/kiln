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
KINDS = ("build", "check", "cheap", "validate", "host-shot", "map-render")


class JobError(ValueError):
    pass


class Job:
    def __init__(self, kind, target, steps, user, arg=None, adapt=None):
        self.id = uuid.uuid4().hex[:12]
        self.kind, self.target, self.user, self.arg = kind, target, user, arg
        self.steps = steps            # [argv, ...]; "{out}" = the previous step's output
        self.argv = steps[-1]
        self.adapt = adapt
        self.report = None
        self.shot = None              # a host-shot job's PNG, once written
        self.stdout = []
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
            "lines": self.seq, "argv": list(self.argv), "arg": self.arg,
            "steps": [list(s) for s in self.steps], "report": self.report,
            "shot": bool(self.shot and Path(self.shot).is_file()),
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
    def __init__(self, repo, nix, manifest, state_dir, max_jobs=2, breaks=(), validators=None):
        self.repo = Path(repo)
        self.nix = nix
        self.manifest = manifest       # callable -> manifest dict
        self.state_dir = Path(state_dir) / "jobs"
        self.state_dir.mkdir(parents=True, exist_ok=True)
        self.sem = threading.Semaphore(max_jobs)
        self.breaks = set(breaks)
        self.validators = validators or {}
        self.jobs = {}
        self.lock = threading.Lock()

    # ── argv, from a fixed menu ───────────────────────────────────────────
    def steps(self, kind, target, arg=None):
        """[argv, ...] for a job, and the adapter for its report (or None)."""
        if kind == "validate":
            spec = self.validators.get(target) if isinstance(target, str) else None
            if spec is None:
                raise JobError(f"no validator {target!r}")
            if "argv" not in self.breaks and arg not in spec["args"]():
                raise JobError(f"{arg!r} is not something {target} can validate here")
            return spec["steps"](arg), (spec["adapt"] or (lambda parsed: parsed))
        if kind == "host-shot":
            return self.host_shot_steps(target), None
        if kind == "map-render":
            # Draw a committed map with the real engine to a PNG — the agent's
            # eyes on a level. Same guard as a validator: a real assets/*.map.
            maps = sorted(f"assets/{p.name}" for p in (self.repo / "assets").glob("*.map"))
            if "argv" not in self.breaks and target not in maps:
                raise JobError(f"{target!r} is not a map in assets/ here")
            return [self._build() + [f"{self.repo}#map-render"],
                    ["{bin}", str(self.repo / target), "{shot}"]], None
        return [self.argv(kind, target)], None

    def _build(self):
        return [self.nix, "build", "--no-link", "--print-out-paths", "--log-format", "internal-json", "-L"]

    def host_shot_steps(self, target):
        """Build a pc-* game, then run it headless — SDL's dummy drivers, a fixed
        number of frames — and keep the frame the real renderer drew with the
        launcher's own --shot. No window, no compositor, nothing on anyone's
        screen, so it is as safe for a remote collaborator as for the host."""
        m = self.manifest()
        if m is None:
            raise JobError("the project manifest is not loaded yet")
        if "argv" not in self.breaks and not (isinstance(target, str) and TARGET_RE.match(target)
                                              and (m.get("packages", {}).get(target) or {}).get("kind") == "pc"):
            raise JobError(f"{target!r} is not a host (pc-*) build in the manifest")
        return [self._build() + [f"{self.repo}#{target}"],
                ["env", "SDL_VIDEODRIVER=dummy", "SDL_AUDIODRIVER=dummy",
                 "{bin}", "--frames", "120", "--shot", "{shot}", "--stats"]]

    def latest_output(self, target):
        """The output path of the most recent successful build of `target` in this session."""
        with self.lock:
            done = [j for j in self.jobs.values()
                    if j.kind == "build" and j.target == target and j.state == "ok" and j.outputs]
        return max(done, key=lambda j: j.ended).outputs[-1] if done else None

    def argv(self, kind, target):
        m = self.manifest()
        if m is None:
            raise JobError("the project manifest is not loaded yet")
        flake = str(self.repo)
        base = self._build()
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
    def submit(self, kind, target, user, arg=None):
        steps, adapt = self.steps(kind, target, arg)
        job = Job(kind, target, steps, user, arg=arg, adapt=adapt)
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
        with self.sem:
            if job.state == "cancelled":
                return self._finish(job)
            job.set_state("running")
            last_out = None
            for i, step in enumerate(job.steps):
                final = i == len(job.steps) - 1
                try:
                    argv = [self._fill(a, job, last_out) for a in step]
                except JobError as e:
                    job.add(str(e))
                    job.exit = 127
                    break
                job.add(f"$ {' '.join(argv)}")
                # A nix build's stdout is its output paths; anything else's is log.
                code = self._step(job, argv, capture=(final and job.adapt is not None) or step[0] != self.nix)
                job.exit = code
                if job.state == "cancelled" or code != 0 and not (final and job.adapt):
                    break
                last_out = job.outputs[-1] if job.outputs else last_out
            if job.adapt is not None and job.stdout:
                try:
                    job.report = job.adapt(json.loads("\n".join(job.stdout)))
                except ValueError as e:
                    job.add(f"the validator's output was not JSON: {e}")
            if job.state != "cancelled":
                ok = job.exit == 0 if job.report is None else bool(job.report.get("ok"))
                job.set_state("ok" if ok and job.exit in (0, 1) and (job.report is not None or job.exit == 0)
                              else "failed")
        self._finish(job)

    def _fill(self, arg, job, out):
        if arg == "{bin}":
            bindir = Path(out or "/nonexistent") / "bin"
            found = sorted(p for p in bindir.iterdir() if p.is_file()) if bindir.is_dir() else []
            if len(found) != 1:
                raise JobError(f"expected exactly one program in {bindir}, found {len(found)}")
            return str(found[0])
        if arg == "{shot}":
            shots = self.state_dir.parent / "shots"
            shots.mkdir(parents=True, exist_ok=True)
            job.shot = str(shots / f"{job.id}.png")
            return job.shot
        return arg.replace("{out}", out or "")

    def _step(self, job, argv, capture):
        try:
            job.proc = subprocess.Popen(
                argv, cwd=self.repo, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, bufsize=1, start_new_session="cancel-group" not in self.breaks)
        except OSError as e:
            job.add(f"could not start: {e}")
            return 127

        def pump_out():
            for line in job.proc.stdout:
                if capture:
                    if job.adapt is not None:
                        job.stdout.append(line.rstrip("\n"))
                    job.add(line.rstrip("\n"))
                    continue
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
        code = job.proc.wait()
        for r in readers:
            r.join(timeout=5)
        return code

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
