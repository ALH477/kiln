# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-voxmesh.nix — Forge's atlas never reaches the screen, and
# this is the proof.
#
# The check renders a real voxel mesh — kiln_voxel's greedy surface extraction
# through kiln_voxmesh's packer through the host 3D pass — TWICE, with two
# colour combiners, and diffs both captures. The pair is the point:
#
#   refs/kiln-voxmesh-shade.png     what Forge draws today: a featureless
#                                   white slab. Five different block types,
#                                   indistinguishable, and you cannot even see
#                                   where one block ends and the next begins.
#   refs/kiln-voxmesh-texshade.png  the same geometry with the atlas sampled.
#
# ── The chain, all of it verifiable from the sources ──────────────────
#   kiln_voxmesh.c:103-106   block type N -> atlas tile N-1, via the UVs. The
#                            type goes NOWHERE else.
#   kiln_voxmesh.c:108       vertex colour is DIR_SHADE[dir] — greyscale
#                            per-face brightness, carrying no type at all.
#   forge_geo.c:27           sets T3D_FLAG_TEXTURED, so the RSP emits texture
#                            coordinates.
#   kiln_engine.c:126        kiln_scene_begin sets RDPQ_COMBINER_SHADE, every
#                            frame. Output = vertex colour; the texel is
#                            discarded.
#   tiny3d t3d.c:300         t3d_state_set_drawflags only encodes the RSP
#                            triangle command. It does not touch the combiner.
#   grep                     nothing in Forge/src sets a combiner at all.
#
# So every one of Forge's fifteen block types draws the same grey, PAINT mode's
# CI4 atlas never reaches the framebuffer, and `Z`'s veiled-palette preview
# cannot change the geometry it is supposed to be previewing. The counter
# kiln_host_t3d_counters()->texels_discarded measures it: ~83,000 texels
# sampled and thrown away in one frame.
#
# ── Why this check demonstrates instead of fixing ─────────────────────
# The fix is one rdpq_mode_combiner call and it belongs in Forge, not the
# engine: kiln_voxmesh_draw's own comment says "Sets NO render state: the
# caller has already chosen the combiner", so the engine behaves as designed
# and the caller is the one omitting it. Which combiner, and whether the
# authored palettes still read once it lands, is a judgement about a CRT that
# no host capture settles — it is exactly what Forge's PAINT mode exists to
# answer, on hardware. So this pins the current behaviour and makes the change
# fail loudly when someone makes it, with the two references as the before and
# after.
{ pkgs, engineSrc, platHost, hostMath }:

pkgs.runCommand "check-kiln-voxmesh"
{
  nativeBuildInputs = [ pkgs.gcc ];
  buildInputs = [ pkgs.zlib ];
  meta.description = "the voxel atlas is discarded by the combiner Forge leaves set";
}
  ''
    set -euo pipefail

    gcc -O1 -g -std=gnu2x -Wall -Wextra \
        -I${platHost}/include -I${platHost}/src -I${hostMath}/include \
        -I${engineSrc}/src/kiln \
        -o voxcheck \
        ${./kiln-voxmesh-check.c} \
        ${engineSrc}/src/kiln/kiln_voxel.c \
        ${engineSrc}/src/kiln/kiln_voxmesh.c \
        ${engineSrc}/src/kiln/kiln_engine.c \
        ${engineSrc}/src/kiln/kiln_gui.c \
        \
        $(echo ${platHost}/src/*.c) \
        ${hostMath}/lib/libkilnmath.a -lz -lm

    ./voxcheck shade.png texshade.png

    fail=0
    for pair in "shade.png ${./refs/kiln-voxmesh-shade.png}" \
                "texshade.png ${./refs/kiln-voxmesh-texshade.png}"; do
      set -- $pair
      if ! cmp -s "$1" "$2"; then
        echo "  FAILED: $1 differs from its reference"
        echo "    reference $(stat -c%s "$2") bytes, rendered $(stat -c%s "$1") bytes"
        fail=1
      fi
    done
    if [ $fail -ne 0 ]; then
      echo ""
      echo "If Forge now sets a TEX combiner, the -shade reference is obsolete"
      echo "and that is the good news — regenerate both and look at them."
      exit 1
    fi

    echo "both voxel renders match their references"
    mkdir -p $out && cp shade.png texshade.png $out/
  ''
