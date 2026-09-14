# SPDX-License-Identifier: MIT
#
# nix/checks/agent-env.nix — the python environments the agents run under,
# and the browser tool's offline half.
#
# python313 and not python3: this pin's python3 is 3.14 and CrewAI requires
# <3.14 — evaluating python3Packages.crewai succeeds but the package is marked
# broken there. The browser agent (tools/agents/browser_mcp.py) needs only
# mcp + websockets; the CrewAI crew's heavier env is built and imported in the
# same gate so its version constraints cannot rot unnoticed against a nixpkgs
# bump.
#
# No Chromium here, on purpose — 150 MB for a check, the same reason
# tools/webverify is `./dev` and not a gate. The selftest covers everything
# that needs no browser (origin confinement, ref and key validation, path
# rules), and tools/studio/tests/mcp_server_test.py drives the studio's MCP
# server over real stdio against a stub studio; one mutation proves the
# selftest can fail at all. The python envs themselves come from
# nix/agent-python.nix, the one definition every agent gate shares.
{ pkgs }:

let
  inherit (import ../agent-python.nix { inherit pkgs; }) browserPython crewPython;
in
pkgs.runCommand "check-agent-env" { } ''
  set -euo pipefail
  mkdir -p "$out"
  # CrewAI's import computes a storage dir under $HOME (utilities/paths.py) and
  # mkdirs it at module import; without HOME set, nix's "homeless-shelter"
  # placeholder makes that a permission error before anything is asserted.
  export HOME="$TMPDIR/home"; mkdir -p "$HOME"

  ${browserPython}/bin/python3 ${../../tools/agents/browser_mcp.py} --selftest | tee "$out/browser-selftest.txt"
  grep -q "selftest: ok" "$out/browser-selftest.txt"

  # The studio's MCP server against a stub studio, over real MCP stdio.
  # Copied as a tree: the test finds mcp_server.py by replacing its own
  # /tests/ path segment, which a flat store copy makes a no-op.
  cp -r ${../../tools/studio} studio
  chmod -R u+w studio
  ${browserPython}/bin/python3 studio/tests/mcp_server_test.py | tee "$out/studio-mcp.txt"
  grep -q "mcp_server test: ok" "$out/studio-mcp.txt"

  ${crewPython}/bin/python3 - <<'PY' | tee "$out/crew-imports.txt"
  import crewai, mcp, litellm, anthropic, websockets
  from crewai.mcp import MCPServerStdio
  from crewai.llm import LLM
  print("crew env imports ok")
  PY
  grep -q "imports ok" "$out/crew-imports.txt"

  # Mutation: break the ref grammar, and the selftest must refuse to pass. A
  # check that has never been seen to fail might not be checking anything.
  cp ${../../tools/agents/browser_mcp.py} mutated.py
  chmod u+w mutated.py
  sed -i 's/\^e\\d{1,5}\$/^x\\d{1,5}$/' mutated.py
  if ${browserPython}/bin/python3 mutated.py --selftest > "$out/mutated.txt" 2>&1; then
    echo "FAIL: a broken ref grammar still passed the selftest"; exit 1
  fi
  echo "  ok   a broken ref grammar is caught" | tee "$out/mutated.txt"
''
