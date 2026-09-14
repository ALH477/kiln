# SPDX-License-Identifier: MIT
"""models.py — the ONE table of which model serves which role, and how to reach it.

Two routes, on purpose a short list:

- **Anthropic, native SDK** — ``LLM(model="anthropic/<model>", max_tokens=…)``;
  CrewAI's own provider, the route whose tool-call and structured-output
  handling is load-bearing for the planner, the engine programmer and the
  reviewer.
- **Ollama cloud** — the user's subscription, reached directly over HTTPS, no
  local daemon: ``LLM(model="openai/<model>", base_url="https://ollama.com/v1",
  api_key=…)``. The OpenAI-compatible route and not ``ollama/``: LiteLLM's
  plain-ollama route drops the auth header against ollama.com (401), and its
  tool-call mapping is buggy besides (LiteLLM #24091, CrewAI #4036).

Known limits shape the use: Ollama cloud does not enforce structured outputs
and ``/v1`` has no ``tool_choice``, so Ollama-routed roles are the ones whose
output is prompt-parsed text (content author, validator), and every Ollama
role names `fallback` = the claude route. `./dev agents-smoke` probes each
model × route with and without a tool and records what actually parses into
``$KILN_REPO/.studio/agents/models.json``; `llm_for` reads that table and
refuses an Ollama route the smoke test recorded as not parsing tool calls,
falling back to Claude instead. Absent the file (never smoke-tested), an
Ollama route is still offered — the smoke test informs, it does not gate.

Keys come from files, never from the repo: ANTHROPIC_API_KEY_FILE and
OLLAMA_API_KEY_FILE (sops-nix paths in the container), with the plain env var
as the local-development fallback.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

TELEMETRY_ENV = {
    "CREWAI_DISABLE_TELEMETRY": "true",
    "OTEL_SDK_DISABLED": "true",
    "CREWAI_TRACING_ENABLED": "false",
}

ANTHROPIC_MODEL = "claude-sonnet-4-5"
OLLAMA_BASE_URL = "https://ollama.com/v1"

# role -> {"route": "claude"|"ollama", "model", "fallback"?}. One table: a
# role's model is changed HERE, never at the Crew construction site.
ROLES = {
    "planner":   {"route": "claude", "model": ANTHROPIC_MODEL},
    "engineer":  {"route": "claude", "model": ANTHROPIC_MODEL},
    "reviewer":  {"route": "claude", "model": ANTHROPIC_MODEL},
    "content":   {"route": "ollama", "model": "gpt-oss:120b", "fallback": "anthropic/" + ANTHROPIC_MODEL},
    "validator": {"route": "ollama", "model": "gpt-oss:120b", "fallback": "anthropic/" + ANTHROPIC_MODEL},
}

MAX_TOKENS = 8192


def _key(env_file: str, env_plain: str) -> str | None:
    f = os.environ.get(env_file)
    if f:
        text = Path(f).read_text().strip()
        if text:
            return text
    return os.environ.get(env_plain) or None


def anthropic_key() -> str | None:
    return _key("ANTHROPIC_API_KEY_FILE", "ANTHROPIC_API_KEY")


def ollama_key() -> str | None:
    return _key("OLLAMA_API_KEY_FILE", "OLLAMA_API_KEY")


def smoke_table(repo: Path | None = None) -> dict:
    """The smoke test's record of which model × route actually parses tool
    calls. {} when never run."""
    repo = repo or Path(os.environ.get("KILN_REPO", os.getcwd()))
    f = repo / ".studio" / "agents" / "models.json"
    if f.is_file():
        try:
            return json.loads(f.read_text()).get("models", {})
        except (OSError, json.JSONDecodeError):
            pass
    return {}


def llm_for(role: str, repo: Path | None = None):
    """Build the crewai LLM for a role. Every knob CrewAI defaults to OpenAI
    is set explicitly, so nothing reaches for a key that is not there."""
    from crewai.llm import LLM  # imported lazily: kiln_agents.tools must not need crewai

    stub = os.environ.get("KILN_AGENTS_LLM_STUB")
    if stub:
        # checks.agent-flow's scripted OpenAI-compatible stub — every role
        # routes to it, so a flow can be run offline end to end.
        return LLM(model="openai/kiln-stub", base_url=stub, api_key="stub",
                   max_tokens=MAX_TOKENS)

    spec = ROLES[role]
    route, model = spec["route"], spec["model"]

    if route == "ollama":
        rec = smoke_table(repo).get(f"ollama:{model}")
        if rec and rec.get("tools") is False:
            fallback = spec["fallback"]  # smoke says this model eats tool calls
            return LLM(model=fallback, max_tokens=MAX_TOKENS,
                       api_key=anthropic_key(), is_litellm=False)
        return LLM(model=f"openai/{model}", base_url=OLLAMA_BASE_URL,
                   api_key=ollama_key(), max_tokens=MAX_TOKENS)

    return LLM(model=f"anthropic/{model}", max_tokens=MAX_TOKENS,
               api_key=anthropic_key(), is_litellm=False)


def describe(repo: Path | None = None) -> dict:
    table = smoke_table(repo)
    return {role: {**spec, "smoke": table.get(f"{spec['route']}:{spec['model']}")}
            for role, spec in ROLES.items()}
