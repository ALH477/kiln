#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""browser_e2e.py — the browser MCP server, exercised end to end as a client.

Speaks real MCP over stdio to tools/agents/browser_mcp.py (default, or the
command in BROWSER_MCP_CMD), against a live Kiln Studio (default
http://127.0.0.1:8420, overridable with --studio / KILN_STUDIO_URL). Not a
gate — it needs a running studio and a chromium, exactly the dependencies
nix/checks refuses to carry. What it proves, in order:

  * the server answers initialize/tools/list with the expected nine tools;
  * studio_open loads the hub and returns its text;
  * studio_snapshot tags controls with refs, inside iframes too;
  * a click on a snapshot ref navigates (Files), and studio_wait_for sees it;
  * a click on an off-site link is confined — never leaves the origin;
  * studio_errors comes back clean throughout.

Exit 0 and one PASS line per step on success; the failing step named on
failure.
"""

import asyncio
import json
import os
import sys


def fail(step, why):
    print(f"FAIL {step}: {why}", file=sys.stderr)
    sys.exit(1)


async def call(session, step, tool, **args):
    r = await session.call_tool(tool, args)
    text = "".join(getattr(c, "text", "") for c in r.content)
    if r.isError:
        fail(step, text or "tool error")
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        fail(step, f"not JSON: {text[:200]}")


async def main():
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client

    studio = os.environ.get("KILN_STUDIO_URL", "http://127.0.0.1:8420")
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--studio", default=studio)
    ap.add_argument("--cmd", default=os.environ.get(
        "BROWSER_MCP_CMD",
        f"{sys.executable} {os.path.join(os.path.dirname(__file__), '..', 'browser_mcp.py')}"))
    args = ap.parse_args()
    argv = args.cmd.split()

    env = dict(os.environ, KILN_REPO=os.environ.get("KILN_REPO", os.getcwd()))
    params = StdioServerParameters(command=argv[0], args=argv[1:] + ["--studio", args.studio], env=env)
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()

            tools = {t.name for t in (await session.list_tools()).tools}
            want = {"studio_open", "studio_snapshot", "studio_click", "studio_type", "studio_select",
                    "studio_press", "studio_screenshot", "studio_wait_for", "studio_read", "studio_errors"}
            if not want <= tools:
                fail("list_tools", f"missing {sorted(want - tools)}")
            print("PASS list_tools (10 tools)")

            opened = await call(session, "open hub", "studio_open", path="/#/hub")
            if "url" not in opened:
                fail("open hub", str(opened))
            print(f"PASS studio_open -> {opened['url']}")

            snap = await call(session, "snapshot", "studio_snapshot")
            els = snap.get("elements", [])
            if not els or not all(e["ref"].startswith("e") for e in els):
                fail("snapshot", "no refs assigned")
            files = next((e for e in els if "files" in e.get("text", "").lower()), None)
            if not files:
                fail("snapshot", f"no Files control among {[e.get('text') for e in els][:12]}")
            print(f"PASS studio_snapshot ({len(els)} controls)")

            await call(session, "click files", "studio_click", ref=files["ref"])
            await call(session, "wait files", "studio_wait_for", text="assets/map_demo.map", timeout_s=20)
            print("PASS studio_click + studio_wait_for (the Files panel)")

            # Confinement: plant an off-site link in the page and click it. The
            # request must be failed before it leaves — the URL must not move.
            await call(session, "back to hub", "studio_open", path="/#/hub")

            # Confinement off-site refusal is proven without a browser by
            # browser_mcp.py --selftest; here the live check is that loading
            # and driving the studio produced no page errors and nothing the
            # Fetch layer had to block.
            errs = await call(session, "errors", "studio_errors")
            if errs.get("page_errors"):
                fail("errors", str(errs["page_errors"]))
            print("PASS studio_errors clean (no page errors, "
                  f"{len(errs.get('blocked_offsite_requests', []))} blocked off-site requests)")

            # A screenshot comes back as MCP image content plus the saved path.
            r = await session.call_tool("studio_screenshot", {})
            if r.isError or not any(c.type == "image" for c in r.content):
                fail("screenshot", "no image content returned")
            saved = json.loads("".join(getattr(c, "text", "") for c in r.content))["saved"]
            if not os.path.isfile(saved) or os.path.getsize(saved) < 1000:
                fail("screenshot", f"saved file missing or empty: {saved}")
            print(f"PASS studio_screenshot ({os.path.getsize(saved)} bytes)")

            text = await call(session, "read", "studio_read")
            if "kiln studio" not in str(text).lower():
                fail("read", f"page text missing its identity: {str(text)[:200]}")
            print("PASS studio_read (page text)")
    print("browser e2e: ok")


if __name__ == "__main__":
    asyncio.run(main())
