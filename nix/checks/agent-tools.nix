# SPDX-License-Identifier: MIT
#
# nix/checks/agent-tools.nix — every tool the role agents get, offline, no model.
#
# Proves the licence each tool claims: fs tools refuse paths outside the task
# worktree (including a symlink pointing out), Git answers status but refuses
# push/checkout/branch surgery, StudioJob speaks the studio's exact contract
# to a stub (kind+target as JSON, the studio's own Origin header) and refuses
# an unknown kind, ClaudeWorker's argv are exactly the pinned ones against a
# fake `claude` on PATH. See tools/agents/tests/agent_tools_test.py.
{ pkgs }:

let
  inherit (import ../agent-python.nix { inherit pkgs; }) crewPython;
in
pkgs.runCommand "check-agent-tools"
  {
    # The test builds a real git worktree and exercises the Git tool, which
    # shells out to git — neither exists in the bare sandbox.
    nativeBuildInputs = [ pkgs.git ];
  }
  ''
  set -euo pipefail
  mkdir -p "$out"
  export HOME="$TMPDIR/home"; mkdir -p "$HOME"
  # tools/agents as a tree — the test finds kiln_agents relative to itself.
  cp -r ${../../tools/agents} agents
  chmod -R u+w agents
  ${crewPython}/bin/python3 agents/tests/agent_tools_test.py | tee "$out/agent-tools.txt"
  grep -q "agent-tools test: ok" "$out/agent-tools.txt"
''
