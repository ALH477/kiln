# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-map.nix — a real Quake .map, loaded off the host VFS by the
# real kiln_map.c, installed into the real clip world, and rendered.
#
# This is the payoff for the IO tier: the first time a level in this repo has
# been drawn anywhere but a ROM. It also proves the negative first, because that
# is the failure this project actually had — a missing map must be a loud miss,
# not a silent empty world. On console kiln_map_load returns non-zero rather
# than asserting, an empty clip world makes every trace report "nothing in the
# way", and the composition is silent; CLAUDE.md records that costing PetaByte
# Madness its entire PLAY screen. Here the same mismatch prints the resolved
# path.
#
# ── It found two defects in kiln_map, and pins both ───────────────────
# 1. kiln_map_draw does not render the brush's FACES. A Quake .map gives three
#    points per face and those points define a PLANE — conventionally one unit
#    apart, which is exactly what assets/quake_test.map uses. kiln_map.c:155
#    treats them as face corners, so a 128-unit wall renders as a 1x2-unit
#    patch at one corner. The check measures it: "face 0 spans 2 units on y;
#    the brush spans 129".
#
#    The correct algorithm is already in the repo, on the host side:
#    tools/blender/quake_map.py intersects every triple of a brush's planes and
#    keeps the candidates inside all the others. kiln_map.c does no
#    intersection at all.
#
# 2. kiln_map.c:164 assigns t3d_vert_pack_normal's uint16_t into a uint8_t,
#    discarding the x field and half of y. Three of six face normals come out
#    zero.
#
# What works is the AABB — componentwise min/max of the plane points — and that
# is what every consumer actually uses: kiln_clip_set_world, kiln_room's brush
# install, PLAY. Which is presumably why the rendering was never examined: the
# geometry on screen comes from models, and the brushes are collision. Note the
# AABB inherits the same off-by-one, so a -64..64 brush becomes a -64..65
# collision box.
#
# Neither is fixed here. Real brush CSG is a geometry change that wants console
# verification, and the reference capture is what makes the fix visible when it
# lands: the check asserts today's behaviour and says so when it stops holding.
{ pkgs, engineSrc, platHost, hostMath, streamdbInc, mapAsset }:

pkgs.runCommand "check-kiln-map"
{
  nativeBuildInputs = [ pkgs.gcc ];
  buildInputs = [ pkgs.zlib ];
  meta.description = "a real .map loads off the host VFS, collides, and renders";
}
  ''
    set -euo pipefail

    gcc -O1 -g -std=gnu2x -Wall -Wextra -Werror \
        -I${platHost}/include -I${platHost}/src -I${hostMath}/include \
        -I${streamdbInc} -I${engineSrc}/src/kiln \
        -o mapcheck \
        ${./kiln-map-check.c} \
        ${engineSrc}/src/kiln/kiln_map.c \
        ${engineSrc}/src/kiln/kiln_clip.c \
        ${engineSrc}/src/kiln/kiln_dict.c \
        ${engineSrc}/src/kiln/kiln_engine.c \
        ${engineSrc}/src/kiln/kiln_gui.c \
        ${engineSrc}/src/kiln/kiln_debugdraw.c \
        $(echo ${platHost}/src/*.c) \
        ${hostMath}/lib/libkilnmath.a -lz -lm

    mkdir -p fs && cp ${mapAsset} fs/quake_test.map
    KILN_HOST_DFS=fs ./mapcheck rom:/quake_test.map out.png

    if ! cmp -s out.png ${./refs/kiln-map.png}; then
      echo ""
      echo "FAILED: the rendered map changed."
      echo "  reference $(stat -c%s ${./refs/kiln-map.png}) bytes, "\
           "rendered $(stat -c%s out.png) bytes"
      echo "If kiln_map now does real brush CSG, this reference is obsolete and"
      echo "that is the good news — regenerate it and look at the result."
      exit 1
    fi

    echo "the map matches its reference capture"
    mkdir -p $out && cp out.png $out/
  ''
