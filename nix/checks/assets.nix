# SPDX-License-Identifier: MIT
#
# nix/checks/assets.nix — the asset conversion tools must be deterministic.
#
# nix/assets.nix wraps each tool in a Nix derivation, so once built, Nix's own
# store hashing makes a non-reproducible tool invisible: the derivation is
# memoised, so running it "twice" from the Nix side just returns the same
# cached path without ever re-invoking the tool. That would hide exactly the
# failure mode this check exists to catch — a tool that embeds a timestamp or
# iterates a hash map in address order, which turns every golden-image test
# downstream into a coin flip. So this runs the underlying binaries directly,
# twice, in one sandboxed build, and diffs the output bytes.
{ pkgs, n64Inst }:

pkgs.runCommand "check-assets-deterministic"
{
  nativeBuildInputs = [ n64Inst ];
  meta.description = "asset conversion tools produce byte-identical output across runs";
  cube = ../../assets/cube.gltf;
  logo = ../../assets/logo.png;
  blip = ../../assets/blip.wav;
}
  ''
    set -euo pipefail

    # Runs "$@" with the literal string OUTDIR replaced by "a" then "b", and
    # diffs the two result directories.
    check_tool() {
      local label="$1"; shift
      local args=("$@")
      mkdir -p a b
      local run_a=("''${args[@]//OUTDIR/a}")
      local run_b=("''${args[@]//OUTDIR/b}")
      "''${run_a[@]}"
      "''${run_b[@]}"
      if diff -rq a b >/dev/null; then
        echo "  $label: deterministic"
      else
        echo "FAIL: $label produced different output across runs:" >&2
        diff -rq a b >&2 || true
        exit 1
      fi
      rm -rf a b
    }

    echo "── model (gltf_to_t3d) ──"
    check_tool "gltf_to_t3d" gltf_to_t3d "$cube" OUTDIR/cube.t3dm --bvh --base-scale=64 --ignore-materials

    echo "── sprite (mksprite) ──"
    check_tool "mksprite" mksprite --format RGBA32 -o OUTDIR "$logo"

    echo "── sound (audioconv64) ──"
    check_tool "audioconv64" audioconv64 -o OUTDIR "$blip"

    echo "assets determinism check PASSED"
    mkdir -p $out && touch $out/ok
  ''
