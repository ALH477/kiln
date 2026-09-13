# SPDX-License-Identifier: MIT
#
# nix/checks/level-vocab.nix — the level vocabulary has ONE statement.
#
# tools/schema/level_vocab.json holds the entity classnames, their epairs, the
# engine's capacities and the canonical AABB face winding. Before it, the same
# facts lived in six hand-maintained copies with nothing comparing any pair:
#
#   tools/mapmaker/src/entity.js   ENTITY_PALETTE      13 classnames
#   tools/mapmaker/src/main.js     EPAIR_SCHEMAS        5, typed
#   tools/mapmaker/validate.py     required_epairs      5, + LIMITS
#   tools/blender-mcp/server.py    a 14-way elif chain, a duplicate of the above
#   Forge/src/forge_ent.c          CLASSNAMES           8, + a DISJOINT epair set
#   engine/src/kiln/kiln_map.c     MAX_*                the same numbers again
#
# They had drifted, and the drift was invisible: forge_ent.c's comment claimed
# to mirror entity.js while holding 8 of its 13; examples/cinematic-demo
# registered info_droid and info_alien, which the editor had never heard of, so
# assets/hangar.map could only be edited through a free-text prompt.
#
# ── Phase A regenerates and diffs, the kiln-font.nix shape ─────────────
# ── Phase B asserts what a diff cannot ────────────────────────────────
# A diff proves the artifacts match the JSON. It cannot prove the JSON is RIGHT,
# and it cannot see a correct table used incorrectly — which is precisely the
# failure this repo actually had: six of its seven .map files were wound
# inside-out, loaded perfectly on console (kiln_map.c takes min/max of the plane
# points and is indifferent to winding), and yielded ZERO polygons through
# quake_map.py's CSG.
#
# ── Proving it fires, which is the house rule ──────────────────────────
# A check only ever seen to pass might not be checking anything. To see each
# phase go red:
#   A  edit one line of tools/mapmaker/src/vocab.gen.js
#   B1 swap [0,1,0] and [0,0,1] in level_vocab.json's -X corners
#   B2 change one coordinate in Forge/src/forge_map.c's snprintf
#   B3 add kiln_map_register_classname("info_nope", 0) to any example
#   B4 put a literal back in kiln_map.c's #define MAX_BRUSHES
{ pkgs, toolsDir, engineSrc, forgeSrc, examplesDir, mapmakerSrc }:

pkgs.runCommand "check-level-vocab"
{
  nativeBuildInputs = [ pkgs.python3 pkgs.nodejs pkgs.gcc pkgs.diffutils ];
  meta.description = "the level vocabulary has one statement, and it means what it says";
} ''
  cp -r ${toolsDir} tools && chmod -R u+w tools
  export PYTHONDONTWRITEBYTECODE=1

  echo "── A. regenerate and diff ────────────────────────────────────────"
  python3 tools/schema/level_vocab.py \
    --emit-js gen.js --emit-forge gen_forge.h --emit-engine gen_engine.h 2>/dev/null

  fail=0
  cmp_or_fail() {
    if ! diff -u "$2" "$1" > d.txt; then
      echo "FAILED: $3 is out of date with tools/schema/level_vocab.json"
      head -40 d.txt
      echo ""
      echo "Regenerate all three:"
      echo "  python3 tools/schema/level_vocab.py \\"
      echo "    --emit-js     tools/mapmaker/src/vocab.gen.js \\"
      echo "    --emit-forge  Forge/src/forge_vocab.gen.h \\"
      echo "    --emit-engine engine/src/kiln/kiln_levelvocab.h"
      fail=1
    fi
  }
  cmp_or_fail gen.js        ${mapmakerSrc}/vocab.gen.js        "tools/mapmaker/src/vocab.gen.js"
  cmp_or_fail gen_forge.h   ${forgeSrc}/forge_vocab.gen.h      "Forge/src/forge_vocab.gen.h"
  cmp_or_fail gen_engine.h  ${engineSrc}/kiln/kiln_levelvocab.h "engine/src/kiln/kiln_levelvocab.h"

  if ! diff -q gen_forge.h ${forgeSrc}/forge_vocab.gen.h >/dev/null 2>&1; then
    echo ""
    echo "NOTE: if the classname ORDER changed, every .FRG already on an SD card"
    echo "      decodes with the wrong classnames — the v2 tail stores"
    echo "      'u8 classname' as an index into that list. Append, or bump"
    echo "      FRG_VERSION in Forge/src/forge_io.c and tools/forge/frg.py."
  fi
  [ "$fail" = 0 ] || exit 1
  echo "  three generated artifacts match the schema"

  echo ""
  echo "── B. what a diff cannot assert ──────────────────────────────────"
  cp -r ${examplesDir} examples
  cp ${engineSrc}/kiln/kiln_map.c kiln_map.c
  cp ${forgeSrc}/forge_map.c ${forgeSrc}/forge_map.h ${forgeSrc}/forge_vocab.gen.h .

  cat > probe.c <<'EOF'
  #include "forge_map.h"
  #include <stdio.h>
  int main(void) {
      const int mn[3] = { -64, -32, -64 }, mx[3] = { 64, 32, 64 };
      char buf[4096];
      forge_map_emit_box(buf, sizeof buf, mn, mx, "TEX");
      fputs(buf, stdout);
      return 0;
  }
EOF
  gcc -std=gnu11 -Wall -Wextra -Werror -I. -o probe probe.c forge_map.c
  ./probe > c_brush.txt

  python3 ${./level-vocab-check.py} \
    --repo . --c-brush c_brush.txt --examples examples --kiln-map kiln_map.c

  mkdir -p $out
  echo ok > $out/result
''
