#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""browser_mcp.py — Kiln Studio, operated through a real browser, as MCP tools.

The UI agent's hands: a headless Chromium driven over the DevTools protocol,
pointed at one Kiln Studio and nothing else, offered to an agent — a CrewAI role
or a Claude Code worker — as the handful of things a person does with it. Open a
panel, look at what is on it, click, type, pick an option, press a key, take a
screenshot, wait for something to appear, read the page's errors.

Deliberately not offered: evaluating JavaScript. An agent that can run script in
the page can do anything the session can without touching the interface it is
supposed to be exercising, and "the save button works" is only evidence when
the save button is what was pressed. Every tool here is an input a person could
give or an observation a person could make. Clicks and keys are real input
events at real coordinates, so a button hidden under an overlay is not clicked.

Confined to the studio's origin. Every request the page makes — navigations,
fetches, frames — is intercepted, and anything addressed elsewhere is failed
before it leaves, so a link, a redirect or an injected resource cannot walk the
session off-site.

Elements are addressed by ref. `studio_snapshot` tags each visible control — on
the page and inside the same-origin map-maker, poser and game frames — with a
short id (e1, e2, ...), and `studio_click`/`studio_type`/`studio_select` take
that id. Each snapshot reassigns them.

    browser_mcp.py --studio http://127.0.0.1:8420 [--token-file F] [--chromium PATH] [--shots DIR]
    browser_mcp.py --selftest            the parts that need no browser
"""

import argparse
import asyncio
import json
import os
import re
import shutil
import sys
import tempfile
import time
import urllib.request
from pathlib import Path
from urllib.parse import urlsplit

REF_RE = re.compile(r"^e\d{1,5}$")
MAX_TEXT = 8000

# DOM `code` values the web launcher's key map uses (plat/shell/shell_web.c),
# plus the keys a form needs. name -> (key, code, windowsVirtualKeyCode).
KEYS = {
    **{c: (c.lower(), f"Key{c}", ord(c)) for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"},
    **{str(d): (str(d), f"Digit{d}", 48 + d) for d in range(10)},
    "Enter": ("Enter", "Enter", 13), "Escape": ("Escape", "Escape", 27), "Tab": ("Tab", "Tab", 9),
    "Backspace": ("Backspace", "Backspace", 8), "Space": (" ", "Space", 32),
    "ArrowUp": ("ArrowUp", "ArrowUp", 38), "ArrowDown": ("ArrowDown", "ArrowDown", 40),
    "ArrowLeft": ("ArrowLeft", "ArrowLeft", 37), "ArrowRight": ("ArrowRight", "ArrowRight", 39),
    "Comma": (",", "Comma", 188), "Period": (".", "Period", 190), "Shift": ("Shift", "ShiftLeft", 16),
}


# ── the parts that need no browser ───────────────────────────────────────────
def allowed_url(base, url):
    """Whether the page may load `url` while pointed at the studio at `base`."""
    if url.startswith(("data:", "blob:", "about:blank")):
        return True
    b, u = urlsplit(base), urlsplit(url)
    if u.scheme not in ("http", "https") or not u.hostname:
        return False
    default = {"http": 80, "https": 443}
    return (u.scheme, u.hostname.lower(), u.port or default[u.scheme]) == \
           (b.scheme, (b.hostname or "").lower(), b.port or default[b.scheme])


def studio_path(path):
    """A path inside the studio ("/#/hub", "/#/map/assets/x.map"), or ValueError."""
    if not isinstance(path, str) or not path.startswith("/") or path.startswith("//") or "\\" in path:
        raise ValueError("give a path inside the studio, starting with /, e.g. /#/hub")
    return path


def valid_ref(ref):
    if not isinstance(ref, str) or not REF_RE.match(ref):
        raise ValueError("refs look like e12 — take one from studio_snapshot")
    return ref


def key_spec(name):
    if name not in KEYS:
        raise ValueError(f"unknown key {name!r}; one of: {', '.join(sorted(KEYS))}")
    return KEYS[name]


def clip(text, n=MAX_TEXT):
    text = text or ""
    return text if len(text) <= n else text[:n] + f"… [{len(text) - n} more characters]"


def selftest():
    fails = []

    def expect(cond, msg):
        if not cond:
            fails.append(msg)

    base = "http://127.0.0.1:8420"
    for url, want in (("http://127.0.0.1:8420/#/hub", True), ("http://127.0.0.1:8420/api/jobs", True),
                      ("http://127.0.0.1/", False), ("http://127.0.0.1:8421/", False),
                      ("https://127.0.0.1:8420/", False), ("http://localhost:8420/", False),
                      ("https://example.com/", False), ("ws://127.0.0.1:8420/", False),
                      ("javascript:alert(1)", False), ("file:///etc/passwd", False),
                      ("data:image/png;base64,AAAA", True), ("blob:http://127.0.0.1:8420/x", True)):
        expect(allowed_url(base, url) is want, f"allowed_url({url!r}) should be {want}")
    expect(allowed_url("https://nixos.taile2de2b.ts.net", "https://nixos.taile2de2b.ts.net:443/x"), "default port")
    for p in ("https://example.com", "//example.com/", "#/hub", "", None, "/\\evil"):
        try:
            studio_path(p)
            fails.append(f"studio_path accepted {p!r}")
        except ValueError:
            pass
    expect(studio_path("/#/map/assets/level.map") == "/#/map/assets/level.map", "a studio path")
    for r in ("e1", "e99999"):
        expect(valid_ref(r) == r, f"ref {r}")
    for r in ('e1"]', "x1", "e", "e123456", 7):
        try:
            valid_ref(r)
            fails.append(f"valid_ref accepted {r!r}")
        except ValueError:
            pass
    expect(key_spec("W") == ("w", "KeyW", 87) and key_spec("Space")[1] == "Space", "key map")
    expect(clip("x" * 10, 4).startswith("xxxx…"), "clip")
    for f in fails:
        print("  FAIL", f)
    print("browser-agent selftest: " + ("FAILED" if fails else "ok"))
    return 1 if fails else 0


# ── in-page scripts (return plain data; never take agent input unquoted) ─────
# FIND's helper is a `var`, not a `const`: top-level lexical declarations of a
# Runtime.evaluate script persist in the page's global lexical environment, so
# a `const` here throws "already declared" the second time any FIND-bearing
# script runs without a full navigation in between.
FIND = r"""
var kilnFind = (ref) => {
  const q = (doc) => {
    const el = doc.querySelector('[data-kiln-ref="' + ref + '"]');
    if (el) return el;
    for (const f of doc.querySelectorAll("iframe")) {
      try { const hit = f.contentDocument && q(f.contentDocument); if (hit) return hit; } catch (e) {}
    }
    return null;
  };
  return q(document);
};
"""

SNAPSHOT = r"""(() => {
  const out = []; let n = 0; const texts = [];
  const visible = (el) => {
    const r = el.getBoundingClientRect();
    if (r.width < 1 || r.height < 1) return false;
    const s = el.ownerDocument.defaultView.getComputedStyle(el);
    return s.visibility !== "hidden" && s.display !== "none";
  };
  const label = (el) => (el.getAttribute("aria-label") || el.innerText || el.getAttribute("placeholder")
    || el.getAttribute("title") || el.getAttribute("alt") || "").trim().replace(/\s+/g, " ").slice(0, 80);
  const walk = (doc, frame) => {
    texts.push(doc.body ? doc.body.innerText : "");
    const sel = 'a[href], button, input, select, textarea, summary, canvas, iframe, [role=button], [contenteditable="true"]';
    for (const el of doc.querySelectorAll(sel)) {
      if (!visible(el)) continue;
      const ref = "e" + (++n);
      el.setAttribute("data-kiln-ref", ref);
      const item = { ref, tag: el.tagName.toLowerCase(), text: label(el) };
      if (el.type && el.tagName !== "BUTTON") item.type = el.type;
      if (el.tagName === "SELECT") {
        item.value = el.value;
        item.options = [...el.options].slice(0, 40).map((o) => o.value);
      } else if (el.tagName === "INPUT" || el.tagName === "TEXTAREA") {
        item.value = String(el.value).slice(0, 80);
      }
      if (el.disabled) item.disabled = true;
      if (frame) item.frame = frame;
      out.push(item);
      if (el.tagName === "IFRAME") { try { if (el.contentDocument) walk(el.contentDocument, ref); } catch (e) {} }
    }
  };
  walk(document, null);
  return { url: location.href, title: document.title, elements: out.slice(0, 400), text: texts.join("\n") };
})()"""

CENTER = FIND + r"""(() => {
  const el = kilnFind(REF);
  if (!el) return null;
  el.scrollIntoView({ block: "center", inline: "center" });
  const r = el.getBoundingClientRect();
  let x = r.left + r.width / 2, y = r.top + r.height / 2, w = el.ownerDocument.defaultView;
  let box = { x: r.left, y: r.top, width: r.width, height: r.height };
  while (w.frameElement) {
    const fr = w.frameElement.getBoundingClientRect();
    const dx = fr.left + w.frameElement.clientLeft, dy = fr.top + w.frameElement.clientTop;
    x += dx; y += dy; box.x += dx; box.y += dy;
    w = w.parent;
  }
  return { x, y, box, disabled: !!el.disabled, tag: el.tagName.toLowerCase() };
})()"""

FOCUS_SELECT = FIND + r"""(() => {
  const el = kilnFind(REF);
  if (!el) return false;
  el.focus();
  if (typeof el.select === "function") el.select();
  return true;
})()"""

SELECT_OPTION = FIND + r"""(() => {
  const el = kilnFind(REF);
  if (!el || el.tagName !== "SELECT") return "not a select";
  if (![...el.options].some((o) => o.value === VALUE)) return "no such option";
  el.value = VALUE;
  el.dispatchEvent(new Event("input", { bubbles: true }));
  el.dispatchEvent(new Event("change", { bubbles: true }));
  return "ok";
})()"""

READ = FIND + r"""(() => {
  if (REF === null) {
    const texts = [];
    const walk = (doc) => { texts.push(doc.body ? doc.body.innerText : "");
      for (const f of doc.querySelectorAll("iframe")) { try { if (f.contentDocument) walk(f.contentDocument); } catch (e) {} } };
    walk(document);
    return texts.join("\n");
  }
  const el = kilnFind(REF);
  return el ? (el.innerText || el.value || el.textContent || "") : null;
})()"""


class ToolError(Exception):
    pass


class Browser:
    def __init__(self, studio, token, chromium, shots):
        self.base = studio.rstrip("/")
        self.token = token
        self.chromium = chromium
        self.shots = Path(shots)
        self.proc = self.ws = self.reader = None
        self.pending = {}
        self.seq = 0
        self.errors = []
        self.blocked = []
        self.shot_n = 0
        self.lock = asyncio.Lock()
        self.profile = None

    # ── lifecycle ─────────────────────────────────────────────────────────
    async def start(self):
        import websockets
        self.profile = tempfile.mkdtemp(prefix="kiln-browser-")
        args = [self.chromium, "--headless=new", "--no-first-run", "--no-default-browser-check",
                "--disable-background-networking", "--disable-sync", "--disable-extensions",
                "--use-angle=swiftshader", "--enable-unsafe-swiftshader",
                "--remote-debugging-port=0", f"--user-data-dir={self.profile}", "--window-size=1400,950",
                "about:blank"]
        if hasattr(os, "geteuid") and os.geteuid() == 0:
            args.insert(1, "--no-sandbox")      # a container's root cannot use Chromium's sandbox
        self.proc = await asyncio.create_subprocess_exec(*args, stdout=asyncio.subprocess.DEVNULL,
                                                         stderr=asyncio.subprocess.DEVNULL)
        port_file = Path(self.profile) / "DevToolsActivePort"
        deadline = time.monotonic() + 30
        while not (port_file.exists() and port_file.read_text().strip()):
            if self.proc.returncode is not None or time.monotonic() > deadline:
                raise ToolError("Chromium did not start")
            await asyncio.sleep(0.1)
        port = int(port_file.read_text().split()[0])
        targets = await asyncio.to_thread(
            lambda: json.loads(urllib.request.urlopen(f"http://127.0.0.1:{port}/json/list", timeout=10).read()))
        page = next(t for t in targets if t.get("type") == "page")
        self.ws = await websockets.connect(page["webSocketDebuggerUrl"], max_size=64 << 20)
        self.reader = asyncio.create_task(self._read())
        for method in ("Page.enable", "Runtime.enable", "Log.enable"):
            await self.send(method)
        await self.send("Fetch.enable", {"patterns": [{"urlPattern": "*"}]})
        await self.send("Page.navigate", {"url": f"{self.base}/#token={self.token}"})
        await asyncio.sleep(2)

    async def close(self):
        try:
            if self.ws:
                await self.ws.close()
        finally:
            if self.proc and self.proc.returncode is None:
                self.proc.kill()
                await self.proc.wait()
            if self.profile:
                shutil.rmtree(self.profile, ignore_errors=True)

    async def ensure(self):
        if self.ws is None:
            await self.start()

    # ── protocol ──────────────────────────────────────────────────────────
    async def _read(self):
        async for raw in self.ws:
            msg = json.loads(raw)
            if "id" in msg:
                fut = self.pending.pop(msg["id"], None)
                if fut and not fut.done():
                    fut.set_result(msg)
                continue
            method, params = msg.get("method"), msg.get("params", {})
            if method == "Fetch.requestPaused":
                asyncio.create_task(self._gate(params))
            elif method == "Runtime.exceptionThrown":
                d = params.get("exceptionDetails", {})
                self.errors.append(clip((d.get("exception") or {}).get("description") or d.get("text", "exception"), 500))
            elif method == "Runtime.consoleAPICalled" and params.get("type") in ("error", "assert"):
                self.errors.append(clip(" ".join(str(a.get("value", a.get("description", "")))
                                                 for a in params.get("args", [])), 500))
            elif method == "Log.entryAdded" and params.get("entry", {}).get("level") == "error":
                e = params["entry"]
                self.errors.append(clip(f"{e.get('text', '')} {e.get('url', '')}".strip(), 500))

    async def _gate(self, p):
        url = p["request"]["url"]
        if allowed_url(self.base, url):
            await self.send("Fetch.continueRequest", {"requestId": p["requestId"]})
        else:
            self.blocked.append(url)
            await self.send("Fetch.failRequest", {"requestId": p["requestId"], "errorReason": "BlockedByClient"})

    async def send(self, method, params=None):
        self.seq += 1
        fut = asyncio.get_running_loop().create_future()
        self.pending[self.seq] = fut
        await self.ws.send(json.dumps({"id": self.seq, "method": method, "params": params or {}}))
        msg = await asyncio.wait_for(fut, 60)
        if "error" in msg:
            raise ToolError(f"{method}: {msg['error'].get('message')}")
        return msg.get("result", {})

    async def evaluate(self, expression):
        r = await self.send("Runtime.evaluate", {"expression": expression, "returnByValue": True, "awaitPromise": True})
        if r.get("exceptionDetails"):
            raise ToolError("the page raised: " + clip(json.dumps(r["exceptionDetails"].get("exception", {}).get("description")), 300))
        return r.get("result", {}).get("value")

    # ── tools ─────────────────────────────────────────────────────────────
    async def open(self, path):
        await self.send("Page.navigate", {"url": self.base + studio_path(path)})
        await asyncio.sleep(1.5)
        return await self.snapshot(brief=True)

    async def snapshot(self, brief=False):
        snap = await self.evaluate(SNAPSHOT)
        snap["text"] = clip(snap["text"], 1500 if brief else MAX_TEXT)
        if brief:
            snap["elements"] = len(snap["elements"])
        if self.errors:
            snap["page_errors"] = len(self.errors)
        return snap

    async def _center(self, ref):
        c = await self.evaluate(CENTER.replace("REF", json.dumps(valid_ref(ref))))
        if c is None:
            raise ToolError(f"no element {ref} — take a fresh studio_snapshot")
        return c

    async def click(self, ref):
        c = await self._center(ref)
        if c["disabled"]:
            raise ToolError(f"{ref} is disabled")
        for kind in ("mouseMoved", "mousePressed", "mouseReleased"):
            await self.send("Input.dispatchMouseEvent", {"type": kind, "x": c["x"], "y": c["y"],
                                                         "button": "left", "clickCount": 1})
        await asyncio.sleep(0.6)
        return {"clicked": ref, "tag": c["tag"]}

    async def type(self, ref, text, submit=False):
        if not isinstance(text, str) or len(text) > 4000:
            raise ToolError("text must be a string of at most 4000 characters")
        await self.click(ref)
        if not await self.evaluate(FOCUS_SELECT.replace("REF", json.dumps(valid_ref(ref)))):
            raise ToolError(f"no element {ref}")
        await self.send("Input.insertText", {"text": text})
        if submit:
            await self.press("Enter")
        await asyncio.sleep(0.4)
        return {"typed": len(text), "into": ref, "submitted": submit}

    async def select(self, ref, value):
        expr = SELECT_OPTION.replace("REF", json.dumps(valid_ref(ref))).replace("VALUE", json.dumps(str(value)))
        r = await self.evaluate(expr)
        if r != "ok":
            raise ToolError(f"{ref}: {r}")
        await asyncio.sleep(0.3)
        return {"selected": value, "in": ref}

    async def press(self, key, hold_ms=0):
        k, code, vk = key_spec(key)
        hold_ms = max(0, min(int(hold_ms or 0), 5000))
        down = {"type": "keyDown", "key": k, "code": code, "windowsVirtualKeyCode": vk}
        if len(k) == 1:
            down["text"] = k
        elif k == "Enter":
            down["text"] = "\r"
        await self.send("Input.dispatchKeyEvent", down)
        if hold_ms:
            await asyncio.sleep(hold_ms / 1000)
        await self.send("Input.dispatchKeyEvent", {"type": "keyUp", "key": k, "code": code, "windowsVirtualKeyCode": vk})
        return {"pressed": key, "held_ms": hold_ms}

    async def screenshot(self, ref=None):
        params = {"format": "png"}
        if ref is not None:
            box = (await self._center(ref))["box"]
            params["clip"] = {**box, "scale": 1}
        data = (await self.send("Page.captureScreenshot", params))["data"]
        import base64
        png = base64.b64decode(data)
        self.shots.mkdir(parents=True, exist_ok=True)
        self.shot_n += 1
        path = self.shots / f"shot-{int(time.time())}-{self.shot_n}.png"
        path.write_bytes(png)
        return png, str(path)

    async def wait_for(self, text, timeout_s=30):
        if not isinstance(text, str) or not text:
            raise ToolError("give the text to wait for")
        deadline = time.monotonic() + max(1, min(float(timeout_s), 300))
        while time.monotonic() < deadline:
            if text in (await self.evaluate(READ.replace("REF", "null")) or ""):
                return {"found": text}
            await asyncio.sleep(0.5)
        raise ToolError(f"{text!r} did not appear within {timeout_s} s")

    async def read(self, ref=None):
        target = "null" if ref is None else json.dumps(valid_ref(ref))
        text = await self.evaluate(READ.replace("REF", target))
        if text is None:
            raise ToolError(f"no element {ref}")
        return clip(text)

    def take_errors(self):
        errors, blocked = self.errors, self.blocked
        self.errors, self.blocked = [], []
        return {"page_errors": errors, "blocked_offsite_requests": blocked}


def build_server(browser):
    from mcp.server.fastmcp import FastMCP, Image

    mcp = FastMCP("kiln-studio-browser", instructions=(
        "Operate Kiln Studio through a real browser, as a person would. Start with studio_open, "
        "then studio_snapshot to see the controls and their refs, then click/type/select by ref. "
        "Check studio_errors after anything that should have worked. The browser cannot leave the studio."))

    async def run(coro):
        async with browser.lock:
            try:
                await browser.ensure()
                return await coro()
            except (ToolError, ValueError) as e:
                return {"error": str(e)}

    def dump(obj):
        return json.dumps(obj, ensure_ascii=False)

    @mcp.tool()
    async def studio_open(path: str = "/#/hub") -> str:
        """Go to a studio page: /#/hub, /#/files, /#/validate, /#/jobs, /#/map/assets/<name>.map,
        /#/pose/<model>, /#/game/web-<game>. Returns the page's title, URL and the start of its text."""
        return dump(await run(lambda: browser.open(path)))

    @mcp.tool()
    async def studio_snapshot() -> str:
        """Every visible control on the page and inside the editor and game frames, each with a ref
        (e1, e2, ...) to pass to studio_click / studio_type / studio_select, plus the page's text."""
        return dump(await run(browser.snapshot))

    @mcp.tool()
    async def studio_click(ref: str) -> str:
        """Click the element with this ref (a real mouse click at its centre)."""
        return dump(await run(lambda: browser.click(ref)))

    @mcp.tool()
    async def studio_type(ref: str, text: str, submit: bool = False) -> str:
        """Replace the contents of an input with text, as typed; submit=True presses Enter after."""
        return dump(await run(lambda: browser.type(ref, text, submit)))

    @mcp.tool()
    async def studio_select(ref: str, value: str) -> str:
        """Choose an option of a <select> by its value (studio_snapshot lists the values)."""
        return dump(await run(lambda: browser.select(ref, value)))

    @mcp.tool()
    async def studio_press(key: str, hold_ms: int = 0) -> str:
        """Press a key on whatever has focus — for a game, click its frame first. Keys: A-Z, 0-9, Enter,
        Escape, Tab, Backspace, Space, ArrowUp/Down/Left/Right, Comma, Period, Shift. hold_ms holds it
        down (up to 5000) — the web launcher maps WASD to the stick, arrows to the D-pad, Space to A."""
        return dump(await run(lambda: browser.press(key, hold_ms)))

    @mcp.tool()
    async def studio_screenshot(ref: str | None = None):
        """A PNG of the page, or of one element by ref. Also saved to disk; the path is returned."""
        async def shot():
            png, path = await browser.screenshot(ref)
            return [Image(data=png, format="png"), dump({"saved": path})]
        result = await run(shot)
        return dump(result) if isinstance(result, dict) else result

    @mcp.tool()
    async def studio_wait_for(text: str, timeout_s: float = 30) -> str:
        """Wait until this text appears anywhere on the page or in its frames (a job finishing, a save
        confirming). Errors if it does not appear in time."""
        return dump(await run(lambda: browser.wait_for(text, timeout_s)))

    @mcp.tool()
    async def studio_read(ref: str | None = None) -> str:
        """The text of one element by ref, or of the whole page and its frames."""
        return dump(await run(lambda: browser.read(ref)))

    @mcp.tool()
    async def studio_errors() -> str:
        """Page errors (exceptions, console errors, failed loads) and off-site requests that were
        blocked, since the last call."""
        return dump(await run(lambda: asyncio.sleep(0, browser.take_errors())))

    return mcp


def main(argv):
    ap = argparse.ArgumentParser(prog="browser_mcp.py", description=__doc__.split("\n\n")[0])
    ap.add_argument("--studio", default=os.environ.get("KILN_STUDIO_URL", "http://127.0.0.1:8420"))
    ap.add_argument("--token-file", default=None,
                    help="the studio's session token (default: $KILN_REPO/.studio/token)")
    ap.add_argument("--chromium", default=os.environ.get("KILN_CHROMIUM") or shutil.which("chromium") or "chromium")
    ap.add_argument("--shots", default=None, help="where screenshots are kept (default: $KILN_REPO/.studio/browser)")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    repo = Path(os.environ.get("KILN_REPO", os.getcwd()))
    token_file = Path(args.token_file) if args.token_file else repo / ".studio" / "token"
    token = os.environ.get("KILN_STUDIO_TOKEN") or (token_file.read_text().strip() if token_file.is_file() else "")
    if not token:
        print(f"browser_mcp: no studio token (looked in $KILN_STUDIO_TOKEN and {token_file})", file=sys.stderr)
        return 2
    browser = Browser(args.studio, token, args.chromium, args.shots or repo / ".studio" / "browser")
    try:
        build_server(browser).run("stdio")
    finally:
        asyncio.run(browser.close())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
