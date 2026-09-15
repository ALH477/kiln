# SPDX-License-Identifier: MIT
#
# nix/checks/agent-flow.nix — KilnTaskFlow end to end, offline, no model.
#
# The one non-negotiable property: a flow must be drivable to completion in
# the sandbox with a scripted fake for both brains (an OpenAI-compatible stub
# via KILN_AGENTS_LLM_STUB) and both studios (the same stub answers the job
# API, scripted red-then-green). Drives plan → work(red) → work(green) →
# review → await_human → a persisted re-kick with the human's approval, and
# asserts no step re-ran on resume. Also that importing kiln_agents sets the
# telemetry-off env and that no OpenAI key exists. See
# tools/agents/tests/agent_flow_test.py.
{ pkgs }:

let
  inherit (import ../agent-python.nix { inherit pkgs; }) crewPython;
in
pkgs.runCommand "check-agent-flow"
  {
    # The test drives a real git worktree (init, add, commit) and the flow's
    # Git tool shells out to git — neither exists in the bare sandbox.
    nativeBuildInputs = [ pkgs.git ];
  }
  ''
  set -euo pipefail
  mkdir -p "$out"
  export HOME="$TMPDIR/home"; mkdir -p "$HOME"
  # The tests find kiln_agents relative to themselves (parents[1]), so they
  # need tools/agents as a TREE — a flat store copy of the file alone makes
  # every relative path point into /nix/store's root.
  cp -r ${../../tools/agents} agents
  chmod -R u+w agents
  env -u OPENAI_API_KEY ${crewPython}/bin/python3 \
      agents/tests/agent_flow_test.py | tee "$out/agent-flow.txt"
  grep -q "agent-flow test: ok" "$out/agent-flow.txt"
''
