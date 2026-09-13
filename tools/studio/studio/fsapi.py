# SPDX-License-Identifier: MIT
"""Reading and saving the files the studio's editors work on.

Two people and a crew of agents share one checkout, so a save is a
compare-and-swap: the client sends the sha256 of the text it opened, and a file
that has changed since then is a 409 carrying the current hash, never a silent
overwrite. On top of that, an open editor holds an advisory lock that expires
when its renewals stop (a closed tab, a dropped connection); someone else saving
while it is held gets a 423 unless they explicitly take the lock over.

Only paths matching tools/studio/allow.json are readable or writable here,
matched segment by segment so a `*` never crosses a `/`, and the path must
reach its file without passing through a symlink: `assets/x.map -> ~/.ssh/id`
is refused, not followed.
"""

import fnmatch
import hashlib
import json
import os
import re
import tempfile
import threading
import time
from pathlib import Path

LOCK_TTL = 30.0
MAX_TEXT = 1 << 20
_SEGMENT = re.compile(r"^[A-Za-z0-9_.+-]+$")


class FsError(Exception):
    def __init__(self, status, msg, **extra):
        super().__init__(msg)
        self.status = status
        self.extra = extra


def sha256(data):
    return hashlib.sha256(data).hexdigest()


class Files:
    def __init__(self, repo, allow_file, breaks=()):
        self.repo = Path(repo).resolve()
        self.patterns = json.loads(Path(allow_file).read_text())["patterns"]
        self.breaks = breaks
        self.locks = {}          # path -> {"user", "expires"}
        self.mutex = threading.Lock()

    # ── which files ───────────────────────────────────────────────────────
    def spec_for(self, rel):
        parts = rel.split("/")
        for p in self.patterns:
            glob = p["glob"].split("/")
            if len(glob) == len(parts) and all(fnmatch.fnmatchcase(a, g) for a, g in zip(parts, glob)) \
                    and not any(fnmatch.fnmatchcase(parts[-1], e) for e in p.get("except", [])):
                return p
        return None

    def resolve(self, rel):
        if not isinstance(rel, str) or not rel or len(rel) > 512:
            raise FsError(400, "a repository-relative path is required")
        if "fs-allow" in self.breaks:
            spec = self.spec_for(rel) or {}
        else:
            if not all(_SEGMENT.match(s) and s not in (".", "..") for s in rel.split("/")):
                raise FsError(400, f"{rel!r} is not a plain relative path")
            spec = self.spec_for(rel)
            if spec is None:
                raise FsError(403, f"{rel} is not a file the studio edits")
        target = self.repo / rel
        if "fs-escape" not in self.breaks and target.resolve() != Path(os.path.normpath(target)):
            raise FsError(403, f"{rel} passes through a symlink")
        return target, spec

    def list(self):
        seen, out = set(), []
        for p in self.patterns:
            for f in sorted(self.repo.glob(p["glob"])):
                rel = str(f.relative_to(self.repo))
                if rel in seen or not f.is_file() or self.spec_for(rel) is not p:
                    continue
                try:
                    self.resolve(rel)
                except FsError:
                    continue
                seen.add(rel)
                st = f.stat()
                out.append({"path": rel, "size": st.st_size, "mtime": int(st.st_mtime),
                            "editor": p.get("editor"), "validator": p.get("validator"),
                            "lock": self.lock_info(rel)})
        return out

    # ── locks ─────────────────────────────────────────────────────────────
    def _holder(self, rel):
        lock = self.locks.get(rel)
        if lock and lock["expires"] <= time.monotonic():
            del self.locks[rel]
            lock = None
        return lock

    def lock_info(self, rel):
        with self.mutex:
            lock = self._holder(rel)
        return {"user": lock["user"], "expires_in": round(lock["expires"] - time.monotonic(), 1)} if lock else None

    def lock(self, rel, user, take=False):
        self.resolve(rel)
        with self.mutex:
            lock = self._holder(rel)
            if lock and lock["user"] != user and not take and "lock" not in self.breaks:
                raise FsError(423, f"{rel} is open in {lock['user']}'s editor", lock={"user": lock["user"]})
            self.locks[rel] = {"user": user, "expires": time.monotonic() + LOCK_TTL}
        return self.lock_info(rel)

    def unlock(self, rel, user):
        self.resolve(rel)
        with self.mutex:
            lock = self._holder(rel)
            if lock and lock["user"] == user:
                del self.locks[rel]
                return True
        return False

    # ── read and compare-and-swap write ──────────────────────────────────
    def read(self, rel):
        target, spec = self.resolve(rel)
        if not target.is_file():
            raise FsError(404, f"{rel} does not exist")
        data = target.read_bytes()
        if len(data) > MAX_TEXT:
            raise FsError(413, f"{rel} is larger than the editors take")
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            raise FsError(415, f"{rel} is not UTF-8 text")
        return {"path": rel, "text": text, "sha256": sha256(data), "lock": self.lock_info(rel),
                "editor": spec.get("editor"), "validator": spec.get("validator")}

    def write(self, rel, text, base, user, take=False):
        target, spec = self.resolve(rel)
        if not isinstance(text, str):
            raise FsError(400, "text must be a string")
        data = text.encode("utf-8")
        if len(data) > MAX_TEXT:
            raise FsError(413, "that is larger than the editors take")
        base = base or None
        if base is not None and not (isinstance(base, str) and re.fullmatch(r"[0-9a-f]{64}", base)):
            raise FsError(400, "baseSha256 must be the hex sha256 the file was opened at, or null for a new file")
        if not target.parent.is_dir():
            raise FsError(404, f"{target.parent.relative_to(self.repo)}/ does not exist")
        with self.mutex:
            lock = self._holder(rel)
            if lock and lock["user"] != user and not take and "lock" not in self.breaks:
                raise FsError(423, f"{rel} is open in {lock['user']}'s editor", lock={"user": lock["user"]})
            current = sha256(target.read_bytes()) if target.is_file() else None
            if current != base and "stale-write" not in self.breaks:
                what = "was created" if base is None else ("was deleted" if current is None else "changed")
                raise FsError(409, f"{rel} {what} since you opened it", sha256=current)
            mode = target.stat().st_mode & 0o777 if target.is_file() else 0o644
            fd, tmp = tempfile.mkstemp(dir=target.parent, prefix=".studio-", suffix=".tmp")
            try:
                with os.fdopen(fd, "wb") as f:
                    f.write(data)
                    f.flush()
                    os.fsync(f.fileno())
                os.chmod(tmp, mode)
                os.replace(tmp, target)
            except BaseException:
                Path(tmp).unlink(missing_ok=True)
                raise
            if take or (lock and lock["user"] == user):
                self.locks[rel] = {"user": user, "expires": time.monotonic() + LOCK_TTL}
        return {"path": rel, "sha256": sha256(data), "validator": spec.get("validator")}
