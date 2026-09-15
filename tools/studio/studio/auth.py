# SPDX-License-Identifier: MIT
"""Who is asking, and whether the request is even addressed to us.

Three independent gates, each of which tests/api_test.py disables in turn to
prove it is the thing stopping an attack rather than something incidental:

  * HOST    — the Host header must name this server. A page on any other site
              can make a browser send requests to 127.0.0.1; DNS rebinding makes
              those requests carry the attacker's hostname, and this refuses it.
  * ORIGIN  — a state-changing request must come from a page this server served.
              Cookies ride along on cross-site POSTs; the Origin header does not
              lie about where the page came from.
  * IDENTITY — a token (cookie or bearer), or a tailnet identity. Tailscale's
              `tailscale serve` proxy injects Tailscale-User-Login, and that
              header is believed ONLY on a connection from the proxy's own
              address. From anywhere else it is a string anyone can type.
"""

import hmac
import os
import secrets
from pathlib import Path

COOKIE = "kiln_studio"
DEFAULT_PROXIES = ("127.0.0.1", "::1")


def load_or_create_token(path):
    path = Path(path)
    if path.exists():
        token = path.read_text().strip()
        if token:
            return token
    path.parent.mkdir(parents=True, exist_ok=True)
    token = secrets.token_urlsafe(32)
    # Created 0600 from the start, not chmod'ed after the secret hit the disk.
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(token + "\n")
    return token


def _host_only(host_header):
    h = host_header.strip().lower()
    if h.startswith("["):
        return h.split("]", 1)[0] + "]"
    return h.rsplit(":", 1)[0] if ":" in h else h


def _cookie_value(header, name):
    for part in (header or "").split(";"):
        k, _, v = part.strip().partition("=")
        if k == name:
            return v
    return None


class Auth:
    def __init__(self, token, allow_hosts=(), tailscale_logins=(), trusted_proxies=DEFAULT_PROXIES,
                 breaks=()):
        self.token = token
        self.allow_hosts = {"localhost", "127.0.0.1", "[::1]"} | {h.lower() for h in allow_hosts}
        self.logins = set(tailscale_logins)
        self.proxies = set(trusted_proxies)
        self.breaks = set(breaks)

    # ── the three gates ───────────────────────────────────────────────────
    def host_ok(self, host_header):
        if "host" in self.breaks:
            return True
        return bool(host_header) and _host_only(host_header) in self.allow_hosts

    def origin_ok(self, method, origin, host_header):
        if method in ("GET", "HEAD", "OPTIONS"):
            return True
        if "origin" in self.breaks:
            return True
        if not origin or not host_header:
            return False
        return origin in (f"http://{host_header}", f"https://{host_header}")

    def identify(self, client_ip, headers):
        """(user, via) or None."""
        login = headers.get("Tailscale-User-Login")
        from_proxy = client_ip in self.proxies or "identity-source" in self.breaks
        if login and self.logins and from_proxy:
            return (login, "tailscale") if login in self.logins else None

        presented = None
        auth = headers.get("Authorization", "")
        if auth.startswith("Bearer "):
            presented = auth[7:].strip()
        if presented is None:
            presented = _cookie_value(headers.get("Cookie"), COOKIE)
        if presented is not None and (self.token_matches(presented) or "token" in self.breaks):
            return ("local", "token")
        return None

    def token_matches(self, presented):
        return hmac.compare_digest(presented.encode(), self.token.encode())

    def session_cookie(self):
        return f"{COOKIE}={self.token}; HttpOnly; SameSite=Strict; Path=/"
