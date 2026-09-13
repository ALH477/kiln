# SPDX-License-Identifier: MIT
"""Who is in the studio and what they have open.

Each open studio tab sends a heartbeat naming its panel and file; an entry
nobody has renewed for TTL seconds is gone. No join or leave messages: a tab
that crashes or loses its connection simply stops beating, which is the only
kind of leaving that always happens.
"""

import re
import threading
import time

TTL = 30.0
_CLIENT = re.compile(r"^[A-Za-z0-9-]{8,64}$")
_PANEL = re.compile(r"^[a-z-]{1,24}$")


class Presence:
    def __init__(self):
        self.clients = {}
        self.mutex = threading.Lock()

    def beat(self, user, client, panel, file=None):
        if not isinstance(client, str) or not _CLIENT.match(client):
            raise ValueError("client must be 8-64 letters, digits or dashes")
        if not isinstance(panel, str) or not _PANEL.match(panel):
            raise ValueError("panel must be a short lowercase name")
        if file is not None and not (isinstance(file, str) and 0 < len(file) <= 512):
            raise ValueError("file must be a path or null")
        with self.mutex:
            self.clients[client] = {"user": user, "panel": panel, "file": file, "seen": time.monotonic()}

    def list(self):
        now = time.monotonic()
        with self.mutex:
            for cid in [c for c, v in self.clients.items() if now - v["seen"] > TTL]:
                del self.clients[cid]
            entries = list(self.clients.values())
        return sorted(({"user": v["user"], "panel": v["panel"], "file": v["file"],
                        "seen_ago": round(now - v["seen"], 1)} for v in entries),
                      key=lambda e: (e["user"], e["panel"], e["file"] or ""))
