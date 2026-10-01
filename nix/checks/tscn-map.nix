# SPDX-License-Identifier: MIT
#
# nix/checks/tscn-map.nix — a level authored in Redot survives the trip to the
# console, byte for byte, on every machine.
#
# Three things, and the third is the reason the other two are not enough.
#
#   A. tools/mapmaker/test_tscn_map.py. The conventions (axes, 64 units to the
#      metre, yaw) proven against a fixture AND the plausible WRONG readings
#      asserted to be rejected, the way tools/blender/demonrig.py's
#      verify_convention does. A mirrored level loads, collides and plays; it
#      is only wrong against an intent nothing else in the tree records.
#
#   B. Every brush the converter emits is run through the real CSG and
#      required to come back whole. PetaByte-Madness' assets/pm_lab.map
#      shipped with all 15 brushes wound inside-out -- it collided correctly,
#      played, and drew NOTHING, because kiln_map.c's AABB reduction is
#      winding-indifferent and its face CSG is not. tools/gen_lab_map.py
#      hand-rolled a winding table instead of using the schema's. Nothing
#      caught it for the life of the file.
#
#   C. REGENERATE AND DIFF, which is the bit-for-bit gate. The committed
#      fixtures/redot_level.map is rebuilt from fixtures/redot_level.tscn and
#      compared byte for byte. Run this check on another architecture and a
#      disagreement is a build failure here rather than a level that is subtly
#      in the wrong place on somebody else's machine. Same shape as
#      level-vocab.nix's part A and nix/checks/pm-gen-headers.nix.
#
#      What makes that gate meaningful is that the converter stays inside
#      arithmetic IEEE-754 pins down: +, -, *, / and sqrt are correctly
#      rounded, so the hull solver is exact; libm's transcendentals are NOT,
#      so atan2 and acos are kept off the output path (see tscn_map.py's
#      "Bit-for-bit across architectures"). This check is what would catch it
#      if one crept back in.
#
# Needs no Redot: a .tscn is text, and the fixture is hand-written.
{ pkgs, toolsDir, assetsDir }:

pkgs.runCommand "check-tscn-map"
{
  nativeBuildInputs = [ pkgs.python3 pkgs.diffutils ];
  meta.description = "a Redot-authored level reaches the console byte for byte";
} ''
  cp -r ${toolsDir} tools && chmod -R u+w tools
  cp -r ${assetsDir} assets && chmod -R u+w assets
  export PYTHONDONTWRITEBYTECODE=1

  echo "── A+B. conventions, limits, and no inside-out brush ──────────────"
  python3 tools/mapmaker/test_tscn_map.py --assets assets

  echo ""
  echo "── C. regenerate and diff: the same scene, the same bytes ─────────"
  python3 tools/mapmaker/tscn_map.py \
    --to-map tools/mapmaker/fixtures/redot_level.tscn --out got.map 2>/dev/null

  if ! diff -u tools/mapmaker/fixtures/redot_level.map got.map > d.txt; then
    echo "FAILED: fixtures/redot_level.map is not what the converter now emits."
    head -60 d.txt
    echo ""
    echo "If the change is intended, regenerate the reference:"
    echo "  python3 tools/mapmaker/tscn_map.py \\"
    echo "    --to-map tools/mapmaker/fixtures/redot_level.tscn \\"
    echo "    --out    tools/mapmaker/fixtures/redot_level.map"
    echo ""
    echo "If it is NOT intended and this is a different machine from the one"
    echo "that wrote the reference, this is the cross-architecture failure"
    echo "this check exists for. Suspect a transcendental on the output path."
    exit 1
  fi
  echo "  the reference level rebuilds byte-identical"

  echo ""
  echo "── D. and the trip back out of the editor is lossless ─────────────"
  python3 tools/mapmaker/tscn_map.py --to-tscn got.map --out back.tscn 2>/dev/null
  python3 tools/mapmaker/tscn_map.py --to-map back.tscn --out again.map 2>/dev/null
  # Not byte-equal to got.map: a convex brush comes back out of the editor as
  # its hull rather than as the box-and-rotation it went in as. Geometry is
  # what has to survive, so compare the STATE, and then insist the second lap
  # is a fixed point.
  python3 ${./tscn-map-roundtrip.py} got.map again.map
  python3 tools/mapmaker/tscn_map.py --to-tscn again.map --out back2.tscn 2>/dev/null
  diff -q back.tscn back2.tscn || {
    echo "FAILED: the .tscn the editor gets back is not a fixed point"; exit 1; }
  echo "  geometry, spawns and worldspawn survive; the second lap is stable"

  mkdir -p $out
  echo ok > $out/result
''
