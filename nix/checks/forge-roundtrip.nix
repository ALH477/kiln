# SPDX-License-Identifier: MIT
#
# nix/checks/forge-roundtrip.nix — Forge's content round trip, on the host.
#
# Forge writes two things from the console: a `.FRG` (its own lossless voxel
# state) and a `.MAP` (the derived Quake brushes). Neither can be verified by
# looking at a ROM, and both are the artifacts that end up committed — so they
# get a gate that runs in milliseconds without an emulator.
#
# ── What this actually protects ────────────────────────────────────────
#
# Three things, each of which has a plausible-looking failure mode:
#
#   1. **The container.** encode/decode must be an exact inverse, including the
#      CRC. A `.FRG` that decodes to a slightly different world is a level that
#      quietly changes every time it is opened.
#
#   2. **The greedy box reduction, twice.** tools/forge/frg.py mirrors
#      kiln_voxel_boxes in Python, and the mirror is asserted to produce a
#      partition that covers the solid set exactly once. The C side is asserted
#      the same way in nix/checks/kiln-logic.nix. Two independent statements of
#      one algorithm is the point: this is the code that becomes the collision
#      world, where a double-covered block is a brush the player sticks inside.
#
#   3. **The .map dialect.** `.map -> voxels -> .map` must be byte-identical, and
#      the emitted text must parse under tools/blender/quake_map.py's CSG — which
#      is the strict reader, and the reason the WINDING matters. The console
#      parser componentwise min/max's plane points and so loads anything;
#      quake_map.py needs correct outward normals. assets/oot_test.map is
#      inside-out under that convention today, works fine on console, and cannot
#      be turned into geometry — exactly the defect class this catches.
#
# Same shape as nix/checks/mapmaker-roundtrip.nix, deliberately: that check runs
# the JS emitter twice and diffs for byte-equal idempotency, then validates. The
# two emitters (mapio.js and forge_io.c/frg.py) must agree about the same brush,
# so they are held to the same standard by the same method.
{ pkgs }:

pkgs.runCommand "check-forge-roundtrip"
{
  nativeBuildInputs = [ pkgs.python3 ];
  # Passed wholesale, the same way mapmaker-roundtrip.nix does it and for the
  # same reason: validate.py finds tools/blender through its own __file__'s
  # parent, so it needs a real tools tree rather than two copied files.
  toolsDir = ../../tools;
  mapFile = ../../assets/quake_test.map;
  meta.description = "Forge .FRG container and .map emitter, asserted on the host";
}
  ''
    set -euo pipefail

    echo "── frg.py selftest ──"
    python3 "$toolsDir/forge/frg.py" selftest

    # And the properties the selftest cannot reach on its own: a real repo .map
    # imported, re-emitted, and validated by the STRICT reader.
    echo
    echo "── against a committed .map ──"
    frg="$toolsDir/forge/frg.py"
    python3 "$frg" frommap "$mapFile" rt.frg
    python3 "$frg" info rt.frg
    python3 "$frg" tomap rt.frg rt1.map
    python3 "$frg" frommap rt1.map rt2.frg
    python3 "$frg" tomap rt2.frg rt2.map

    if ! diff -u rt1.map rt2.map; then
      echo "forge-roundtrip: emit is not idempotent — a save would look like a" \
           "change every time" >&2
      exit 1
    fi
    echo "emit is idempotent"

    # The strict reader. This is the half that catches a wrong winding, which
    # loads fine on console and produces no geometry at all off it.
    echo
    echo "── quake_map.py CSG over the emitted .map ──"
    python3 "$toolsDir/mapmaker/validate.py" rt1.map | tee validate.txt
    grep -q "OK" validate.txt || {
      echo "forge-roundtrip: validate.py did not report OK" >&2; exit 1; }

    echo
    echo "forge round-trip check PASSED"
    mkdir -p $out && touch $out/ok
  ''
