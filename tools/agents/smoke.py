#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""smoke.py — `./dev agents-smoke`: which model × route actually parses tool calls.

NOT a gate — it spends subscription credits, and it is the one place that is
allowed to. For every role's configured model it makes two tiny requests:

  1. plain    — "reply with the word pong": proves reachability and auth;
  2. tools    — the same question with one no-op tool offered and
                tool_choice set where the route allows it: proves the route
                survives CrewAI/litellm's tool-call mapping (the known-buggy
                part of the Ollama-via-/v1 path).

plus a real OpenAI-compat round trip per route family. The result is written
to $KILN_REPO/.studio/agents/models.json:

  {"ollama:gpt-oss:120b": {"plain": true, "tools": false}, ...}

which models.llm_for reads: a route whose tools entry is false is not given
to a role that must call tools — the fallback (Claude) is used instead. Run
it after changing a model in models.ROLES, and once against a fresh
subscription before trusting an Ollama role.
"""

import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import kiln_agents  # noqa: F401 — telemetry off before crewai is imported
from kiln_agents import models

TOOL = [{"type": "function", "function": {
    "name": "noop", "description": "reply by calling this with answer='pong'",
    "parameters": {"type": "object",
                   "properties": {"answer": {"type": "string"}}, "required": ["answer"]}}}]
MESSAGES = [{"role": "user", "content": "Call the noop tool with answer='pong'."}]


def probe_openai_compat(base_url: str, api_key: str, model: str, with_tools: bool) -> bool:
    """One raw round trip — no litellm — so route reachability is measured
    separately from litellm's tool-call mapping."""
    import urllib.request
    body = {"model": model, "messages": MESSAGES if with_tools else
            [{"role": "user", "content": "Reply with exactly: pong"}], "max_tokens": 32}
    if with_tools:
        body["tools"] = TOOL
    req = urllib.request.Request(
        base_url.rstrip("/") + "/chat/completions",
        data=json.dumps(body).encode(),
        headers={"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=90) as r:
            data = json.loads(r.read())
    except Exception as e:  # auth wrong, model missing, network — all "route broken"
        print(f"      openai-compat {model} tools={with_tools}: {e}")
        return False
    msg = data["choices"][0]["message"]
    if with_tools:
        return bool(msg.get("tool_calls"))
    return "pong" in (msg.get("content") or "").lower()


def probe_crewai(llm) -> bool:
    """The same question through CrewAI's own LLM wrapper — where the tool
    mapping bugs actually live."""
    try:
        out = llm.call(MESSAGES, tools=TOOL)
    except Exception as e:
        print(f"      crewai call: {e}")
        return False
    if isinstance(out, str):
        return "pong" in out.lower()
    # tuple/list of tool calls
    try:
        return any("pong" in json.dumps(c).lower() for c in out)
    except TypeError:
        return False


def main() -> int:
    repo = Path(os.environ.get("KILN_REPO", os.getcwd()))
    results: dict[str, dict] = {}
    t0 = time.time()

    # Anthropic native (one probe for the family)
    if models.anthropic_key():
        llm = models.llm_for("planner", repo)
        ok_plain = "pong" in str(llm.call([{"role": "user", "content": "Reply with exactly: pong"}])).lower()
        results[f"claude:{models.ANTHROPIC_MODEL}"] = {
            "plain": ok_plain, "tools": probe_crewai(llm)}
        print(f"  claude:{models.ANTHROPIC_MODEL}: {results[f'claude:{models.ANTHROPIC_MODEL}']}")
    else:
        print("  claude: NO ANTHROPIC KEY — skipped")

    # Ollama cloud via /v1 (probe each distinct configured model once)
    if models.ollama_key():
        for model in sorted({s["model"] for s in models.ROLES.values() if s["route"] == "ollama"}):
            plain = probe_openai_compat(models.OLLAMA_BASE_URL, models.ollama_key(), model, False)
            tools_raw = probe_openai_compat(models.OLLAMA_BASE_URL, models.ollama_key(), model, True)
            rec = {"plain": plain, "tools_raw": tools_raw}
            if plain:
                from crewai.llm import LLM
                llm = LLM(model=f"openai/{model}", base_url=models.OLLAMA_BASE_URL,
                          api_key=models.ollama_key(), max_tokens=64)
                rec["tools"] = probe_crewai(llm)
            else:
                rec["tools"] = False
            results[f"ollama:{model}"] = rec
            print(f"  ollama:{model}: {rec}")
    else:
        print("  ollama: NO OLLAMA KEY — skipped")

    out = repo / ".studio" / "agents" / "models.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"recorded": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                               "models": results}, indent=2) + "\n")
    print(f"wrote {out.relative_to(repo)} in {time.time() - t0:.0f}s")
    print("agents-smoke: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
