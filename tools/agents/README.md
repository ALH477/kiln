<!-- SPDX-License-Identifier: MIT -->

# tools/agents — the agents

Two layers live here, both facing Kiln Studio rather than the repo directly:

- **`kiln_agents/` — the CrewAI runtime.** One flow per task: a brief comes in
  (from the studio's Agents panel, or `POST /task` on the agents service), a
  planner assigns it a kind and checks, a role agent works it in its own git
  worktree, a validator runs the studio's checks over the branch, a reviewer
  reads the diff, and the flow **persists and waits for a human's
  approve/reject** before anything merges. `serve.py` is the small stdlib HTTP
  service that runs those flows (one process, per-task state in SQLite via
  CrewAI's `@persist`, so an approval hours later resumes the same flow
  without re-planning).
- **`browser_mcp.py` — the UI agent.** Kiln Studio operated through a real
  browser, exposed as MCP tools: a headless Chromium driven over the DevTools
  protocol, pointed at one studio and nothing else, offering an agent (a
  CrewAI role or a Claude Code worker) the handful of things a person does
  with the interface — open a panel, see what is on it, click, type, pick an
  option, press a key, take a screenshot, wait for something to appear, read
  the page's errors.

## The crew and its rules

`kiln_agents/agents.yaml` holds five roles; `tools/__init__.py`'s
`role_tools` gives each one only what its job needs:

| role | model (models.py) | tools |
|---|---|---|
| planner | Claude | read-only files, studio |
| engineer | Claude | everything, incl. the Claude Code worker |
| content | Ollama cloud | files+git+studio (levels via `./dev map-emit` specs, never hand-written `.map`) |
| validator | Ollama cloud | read+grep+studio ("the verdict is the reports' own `ok`") |
| reviewer | Claude | read+grep+git+studio, no write ("read diffs, not summaries") |

The rules the tools enforce, rather than document:

- **Worktree scope is checked, not requested.** Every file tool resolves its
  path and refuses anything outside the task's worktree (`tools/_scope.py`) —
  `../`, absolute paths, and symlinks pointing out all fail the same way.
- **Git is a five-op allowlist** (`status`, `diff`, `log`, `add`, `commit`).
  Push, checkout and everything else return a refusal.
- **Builds go through the studio.** `StudioJob` posts to the studio API and
  waits; an agent's build sits in the same semaphore-capped queue and SSE log
  as a person's, and the validator role reads the reports' own `ok`.
- **A role may hand a task to a Claude Code worker** (`ClaudeWorker`) — the
  headless CLI with `acceptEdits` and an allowlist of tools, cwd in the
  worktree, prompt over stdin, budget in minutes. CrewAI decides *what*;
  Claude Code does the large edits it is better at.
- **Telemetry is off before anything imports crewai** (`__init__.py` sets the
  env), and there is no OpenAI key anywhere — absence is the guard against
  CrewAI's OpenAI-phoning defaults.

Models are one table in `models.py`: Claude where tool-calling must parse,
Ollama cloud (`openai/<model>` against `https://ollama.com/v1`) where prose is
enough, with Claude as the per-role fallback. `./dev agents-smoke` spends a
few credits to find out which model × route *actually* parses tool calls and
writes `.studio/agents/models.json` — a route recorded as `tools:false` is
skipped straight to its fallback, because a role that cannot call tools is a
role that talks about work instead of doing it. Run the smoke after changing
a model in the table, and once against a fresh subscription.

## The loop, end to end

1. A person types a brief in the studio's **Agents** panel `#/agents`
   (or `POST /api/agent-tasks`). The studio creates a worktree
   (`.studio/agents/worktrees/<id>`, branch `agent/<id>`) and hands the task
   to the agents service.
2. The flow plans, works, validates (red loops back to work, capped at three
   rounds), and reviews — the studio's panel shows each phase as it's
   PATCHed back.
3. Green means **awaiting a human**, not merged. The panel shows the plan,
   the validation reports, the review and the branch's diff stat; approve or
   reject there. Merge is fast-forward-only, then the worktree and branch are
   removed; reject keeps everything for inspection until discarded.

Every agent artefact lives under `.studio/agents/` (gitignored; per machine):
`models.json` (the smoke results), `flows.db` (flow state, what an approval
resumes), `tasks.json` (the studio's records), `worktrees/`.

## Checking them

- `nix build .#checks.x86_64-linux.agent-env` — the python environments (the
  browser tool's, and the CrewAI crew's) plus the browser tool's offline
  `--selftest`, with one mutation to prove the selftest fires.
- `.#checks...agent-tools` — every tool's refusal, offline: paths outside the
  worktree, symlink escapes, git's forbidden ops, `StudioJob` against a stub
  studio, `ClaudeWorker`'s argv pinned against a fake `claude`. No model is
  called.
- `.#checks...agent-flow` — the whole flow against a scripted stub LLM
  (plan → work → validate red → work again → green → awaiting human), then a
  re-kick with the approval, asserting the resume did not re-run a single
  model call. Also asserts the telemetry env and the absence of any OpenAI
  key.
- `./dev agents-smoke` — the one command allowed to spend credits; see above.

## The browser tool in more detail

It exists so that "the studio works" can be tested the way the studio is used.
An agent that edits `assets/map_demo.map` through the map-maker's save button has
proven the save button, the compare-and-swap behind it, and the validator the
save triggers — a `curl` of `/api/fs/write` proves only the last two.

## Setup

`.mcp.json` at the repo root already registers it as `kiln-browser`:

```json
"kiln-browser": { "command": "nix", "args": ["run", ".#browser-mcp"] }
```

Run it directly with `nix run` and never through `./dev` — dev's auto-`nix
develop` shellHook prints a banner to stdout, and stdout is the JSON-RPC
channel (see the `mcp` case in `dev`). `./dev mcp --build` pre-warms the
closure so a first connect does not outrun the client's startup timeout.

The tool needs:

- **a running studio** — `--studio URL` (default `$KILN_STUDIO_URL` or
  `http://127.0.0.1:8420`);
- **its session token** — `$KILN_STUDIO_TOKEN`, else read from
  `$KILN_REPO/.studio/token` (the studio writes it on startup);
- **a chromium** — `--chromium PATH` / `$KILN_CHROMIUM` / `chromium` on PATH;
  the `browser-mcp` app defaults it to the flake's pinned chromium.

## The tools

`studio_open` (a path inside the studio, like `/#/hub` or
`/#/map/assets/map_demo.map`) → `studio_snapshot` (every visible control, on the
page and inside the same-origin editor/game iframes, tagged `e1`, `e2`, …) →
`studio_click` / `studio_type` / `studio_select` / `studio_press` by ref →
`studio_screenshot` (PNG, also saved under `.studio/browser/`) →
`studio_wait_for` (text to appear, e.g. a job finishing) → `studio_read` (page
or element text) → `studio_errors` (page exceptions and blocked off-site
requests since the last call).

Each snapshot reassigns the refs — snapshot again after anything that re-renders.

## The guarantees

- **Confined to the studio's origin.** Every request the page makes —
  navigations, fetches, subframes — is intercepted (`Fetch.enable`), and
  anything addressed elsewhere is failed before it leaves. A link, a redirect
  or an injected resource cannot walk the session off-site.
- **No JavaScript evaluation is offered as a tool.** Clicks and keys are real
  input events at real coordinates, so a button hidden under an overlay is not
  clicked, and a claim about the UI is a claim about the UI.
- **Keys are the launcher's keys.** The key map matches `plat/shell/shell_web.c`'s
  codes — WASD is the stick, arrows the D-pad, Space the A button — so a game
  in the game panel is playable through `studio_press`.

## Checking it

- `nix build .#checks.x86_64-linux.agent-env` — the python environments (the
  browser tool's, and the CrewAI crew's) plus the offline `--selftest`
  (origin confinement table, ref and key validation, path rules), with one
  mutation to prove the selftest fires.
- `tools/agents/tests/browser_e2e.py` — a live end-to-end pass against a
  running studio (list tools, open the hub, snapshot, click into Files, wait
  for the file list, clean error log). Not a gate; it needs a studio and a
  chromium. Run it with the check env's python, e.g.:

  ```bash
  nix run .#studio &   # in another shell
  KILN_REPO=$PWD KILN_CHROMIUM=$(nix build --no-link --print-out-paths nixpkgs#chromium) \
    "$(nix build --no-link --print-out-paths --impure --expr \
      'with builtins.getFlake (toString ./.).inputs.nixpkgs.legacyPackages.x86_64-linux;
       python313.withPackages (ps: [ ps.mcp ps.websockets ])')/bin/python3" \
    tools/agents/tests/browser_e2e.py
  ```
