# SPDX-License-Identifier: MIT
#
# nix/agent-python.nix — the python environments the agents run under.
#
# python313 and not python3: this pin's python3 is 3.14 and CrewAI requires
# <3.14 — evaluating python3Packages.crewai succeeds but the package is
# marked broken there. One definition, imported by every agent check so all
# four gates build the SAME environment derivation (one store realisation).
# The agents container image also takes its interpreter from here, so what
# the gates prove is what the container runs.
#
# crewai here is the pin's crewai with lancedb STRUCK OUT. crewai only needs
# lancedb for its memory subsystem — crewai/memory/__init__.py lazy-imports
# it so `import crewai` never does — and every Crew this repo builds sets
# memory=False, so the runtime never touches it either. Three edits, because
# the dependency sneaks back in at each layer: filter it out of the
# propagated inputs (the closure), pythonRemoveDeps (the wheel's METADATA,
# which pythonRelaxDepsHook rewrites at postBuild — otherwise
# pythonRuntimeDepsCheck fails on the hole we just made), and a recomputed
# passthru.requiredPythonModules (the env's path list, which toPythonModule
# memoises before any of the above). Dropping it at all
# is what keeps the env buildable: on this pin lancedb-0.32 needs
# pylance-8.0.0, a maturin build whose cargo step fails (exit 101). If a
# future task needs crewai memory, either bump nixpkgs past the break or
# bring lancedb back and fix its pylance — the agent-flow gate would catch
# a missing import the moment a flow constructs memory.
{ pkgs }:

let
  inherit (pkgs) lib;
  notLancedb = p: p.pname or "" != "lancedb";
  crewaiNoLance = pkgs.python313Packages.crewai.overrideAttrs (old: {
    propagatedBuildInputs = lib.filter notLancedb old.propagatedBuildInputs;
    pythonRemoveDeps = (old.pythonRemoveDeps or [ ]) ++ [ "lancedb" ];
    # python.withPackages does NOT read propagatedBuildInputs to build the
    # environment's paths: it reads passthru.requiredPythonModules, which
    # toPythonModule memoised over the inputs BEFORE this filter. Without
    # recomputing it here, lancedb lands in the env anyway and the env drv
    # dies on the very build the strike exists to avoid.
    passthru = (old.passthru or { }) // {
      requiredPythonModules =
        pkgs.python313Packages.requiredPythonModules
          (lib.filter notLancedb old.propagatedBuildInputs);
    };
    # Upstream's pytest suite is broken on this pin before it reaches a
    # single test (conftest wants vcr.stubs.httpcore_stubs, which this
    # vcrpy has no module for) — crewai has never finished building here.
    # The agent gates are the verification that matters for THIS repo's
    # use: agent-env imports crewai through this override, agent-flow runs
    # the whole KilnTaskFlow against a stub LLM.
    dontUsePytestCheck = true;
  });
in {
  browserPython = pkgs.python313.withPackages (ps: with ps; [ mcp websockets ]);
  crewPython = pkgs.python313.withPackages (ps: with ps; [
    crewaiNoLance mcp litellm anthropic websockets pyyaml
  ]);
}