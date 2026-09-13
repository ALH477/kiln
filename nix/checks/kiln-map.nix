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
# ── It found three defects in kiln_map, and now pins their fix ────────
# 1. kiln_map_draw did not render the brush's FACES. A Quake .map gives three
#    points per face and those points define a PLANE — conventionally one unit
#    apart, which is exactly what assets/quake_test.map uses. kiln_map.c
#    treated them as face corners, so a 128-unit wall rendered as a 1x2-unit
#    patch at one corner. This check measured it: "face 0 spans 2 units on y;
#    the brush spans 129". It now measures 128.
#
# 2. t3d_vert_pack_normal's uint16_t was assigned into a uint8_t, discarding
#    the x field and half of y. Three of six face normals came out zero.
#
# 3. Found only by fixing (1): the normal was INWARD. cross(p2-p1, p3-p1),
#    where the Quake convention is cross(p3-p1, p2-p1) — quake_map.py's
#    docstring settles the sign against the canonical axial cube. Every brush
#    face in every level had its lighting negated, and nothing could report it
#    while the faces were 1x2-unit patches nobody could see.
#
# All three are fixed. kiln_map.c now ports tools/blender/quake_map.py's CSG:
# intersect every triple of planes, keep the candidates inside all the others,
# weld, order into a ring. The assertions below pin the CORRECT behaviour, and
# include the one a "not zero" test would miss — exactly two faces carry a
# non-zero x field in the packed 5.6.5 normal, which is the half-byte that
# used to be discarded.
#
# The AABB moved with it. It was the componentwise min/max of the plane POINTS,
# so a -64..64 brush became a -64..65 collision box; it is now the true box
# from the CSG vertices. An inside-out brush yields NO vertices, so kiln_map.c
# keeps the plane-point box as a fallback and debugfs ./dev map-canon — without
# that, a winding bug would become a brush with no collision, which reads as
# "the player falls through the world".
#
# ── One body, every architecture ───────────────────────────────────────
# Built by nix/host.nix's `target`: it supplies the compiler, the flags and
# the three archives, so this file says what to render and what to compare
# and nothing about how to compile it. The same body runs under wasm32
# against the SAME reference files — flake.nix declares that variant as
# `<name>-wasm32`; `./dev arch <target>` runs it on the others.
{ pkgs, target, mapAsset, mapRenderSrc }:

target.mkCheck {
  pname = "mapcheck";
  sources = [ ./kiln-map-check.c "${mapRenderSrc}/map_render.c" ];
  extraCFlags = [ "-I${mapRenderSrc}" ];
  args = "rom:/quake_test.map out.png";
  env = "KILN_HOST_DFS=fs";
  meta.description = "a real .map loads off the host VFS, collides, and renders";
  preRun = ''
    mkdir -p fs && cp ${mapAsset} fs/quake_test.map
    # The same cube with ONE face wound backwards (face 0's second and third
    # points swapped). Its CSG keeps a single quad, whose box is zero-thick;
    # kiln-map-check.c asserts the collision box still has volume.
    # Addressed by the face's text, not a line number: the first version said
    # `3s`, line 3 is the brush's `{`, sed changed nothing, and the guard
    # below stopped the build under set -e with no output at all.
    sed '0,/( -64 -64 -64 ) ( -64 -63 -64 ) ( -64 -64 -63 )/s//( -64 -64 -64 ) ( -64 -64 -63 ) ( -64 -63 -64 )/' \
      ${mapAsset} > fs/one_face_flipped.map
    grep -q '( -64 -64 -64 ) ( -64 -64 -63 ) ( -64 -63 -64 )' fs/one_face_flipped.map || {
      echo "kiln-map: could not flip face 0 of ${mapAsset}; its text changed"; exit 1; }
  '';
  script = ''
    if ! cmp -s out.png ${./refs/kiln-map.png}; then
      echo ""
      echo "FAILED: the rendered map changed."
      echo "  reference $(stat -c%s ${./refs/kiln-map.png}) bytes, rendered $(stat -c%s out.png) bytes"
      echo "If kiln_map now does real brush CSG, this reference is obsolete and"
      echo "that is the good news — regenerate it and look at the result."
      exit 1
    fi
    echo "the map matches its reference capture (${target.description})"
  '';
}
