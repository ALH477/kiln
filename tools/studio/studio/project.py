# SPDX-License-Identifier: MIT
"""The project model and the machine's capabilities, loaded once and served.

The manifest comes from the flake (`nix eval --json .#studioManifest.<system>`,
see nix/studio-manifest.nix) and capabilities from `./dev doctor --json`, so the
studio never keeps a second list of games or a second notion of what this
machine can do. Both are slow (an eval; a `nix develop` entry), so they load in
the background and the API answers 503 until they are ready. Tests pass files.
"""

import json
import subprocess
import threading
from pathlib import Path


class Project:
    def __init__(self, repo, nix, manifest_file=None, caps_file=None):
        self.repo = Path(repo)
        self.nix = nix
        self.manifest_file = manifest_file
        self.caps_file = caps_file
        self.manifest = None
        self.caps = None
        self.errors = {}
        self.lock = threading.Lock()

    def load(self, background=True):
        if self.manifest_file:
            self.manifest = json.loads(Path(self.manifest_file).read_text())
        if self.caps_file:
            self.caps = json.loads(Path(self.caps_file).read_text())
        todo = []
        if self.manifest is None:
            todo.append(self._load_manifest)
        if self.caps is None:
            todo.append(self._load_caps)
        for fn in todo:
            if background:
                threading.Thread(target=fn, daemon=True).start()
            else:
                fn()

    def reload_manifest(self):
        if self.manifest_file:
            self.manifest = json.loads(Path(self.manifest_file).read_text())
        else:
            threading.Thread(target=self._load_manifest, daemon=True).start()

    def _run(self, argv, timeout):
        r = subprocess.run(argv, cwd=self.repo, capture_output=True, text=True, timeout=timeout)
        if r.returncode != 0:
            raise RuntimeError((r.stderr or r.stdout).strip()[-2000:])
        return r.stdout

    def _load_manifest(self):
        try:
            system = self._run([self.nix, "eval", "--raw", "--impure", "--expr", "builtins.currentSystem"], 120).strip()
            data = json.loads(self._run([self.nix, "eval", "--json", f"{self.repo}#studioManifest.{system}"], 900))
            with self.lock:
                self.manifest = data
                self.errors.pop("manifest", None)
        except Exception as e:   # surfaced on the hub, not swallowed
            self.errors["manifest"] = str(e)

    def _load_caps(self):
        try:
            out = self._run([str(self.repo / "dev"), "doctor", "--json"], 900)
            line = [l for l in out.splitlines() if l.strip().startswith("{")][-1]
            with self.lock:
                self.caps = json.loads(line)
                self.errors.pop("caps", None)
        except Exception as e:
            self.errors["caps"] = str(e)
