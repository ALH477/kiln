# SPDX-License-Identifier: MPL-2.0
#
# nix/checks/mapmaker-roundtrip.nix — the editor's .map emit and the engine's
# .map parser must agree. Round-trips assets/quake_test.map (the canonical
# file mkQuakeMapModel round-trips through Blender, so quake_map.py's CSG
# accepts its winding) through tools/mapmaker/src/roundtrip.js (parse +
# re-emit canonical form via mapio.js), then through tools/mapmaker/validate.py
# (which reuses quake_map.py's pure-Python CSG), and asserts idempotency
# (parse + re-emit twice = byte-equal).
#
# Why quake_test.map not oot_test.map: oot_test.map is INSIDE-OUT relative to
# the Quake winding convention quake_map.py's CSG requires (the engine itself
# is winding-independent, so oot_test.map still parses on console). The
# editor's emit always uses the canonical winding, so an
# imported-then-exported oot_test.map would round-trip cleanly through this
# check; we use quake_test.map as the seed because it's the file the existing
# mkQuakeMapModel path already validates against.
{ pkgs }:

pkgs.runCommand "check-mapmaker-roundtrip"
{
  nativeBuildInputs = [ pkgs.nodejs pkgs.python3 ];
  mapFile = ../../assets/quake_test.map;
  meta.description = "three.js map maker's .map emit round-trips through the engine parser";
}
  ''
    set -euo pipefail
    cd "${
      builtins.toString ./../..
    }"

    node tools/mapmaker/src/roundtrip.js "$mapFile" "$out/round1.map" > "$out/round1.log"
    node tools/mapmaker/src/roundtrip.js "$out/round1.map" "$out/round2.map" > "$out/round2.log"

    # Idempotency: parse + re-emit twice must produce byte-equal output.
    diff "$out/round1.map" "$out/round2.map" || {
      echo "FAIL: mapio.js emit is not idempotent" >&2
      exit 1
    }
    echo "  idempotency: parse + re-emit twice = byte-equal ✓"

    # validate.py must accept the round-tripped file (it reuses quake_map.py's
    # CSG; degenerate or inside-out winding yields 0 surviving faces).
    python3 tools/mapmaker/validate.py "$out/round1.map" > "$out/validate.log" 2>&1
    cat "$out/validate.log"

    # Sanity: counts must match quake_test.map's known contents.
    grep -q '^brushes:   1 /' "$out/validate.log" || { echo "FAIL: expected 1 brush" >&2; exit 1; }
    grep -q '^spawns:    1 /' "$out/validate.log" || { echo "FAIL: expected 1 spawn" >&2; exit 1; }
    grep -q '^OK$' "$out/validate.log" || { echo "FAIL: validate.py did not report OK" >&2; exit 1; }

    echo "mapmaker round-trip check PASSED"
    touch "$out/ok"
  ''