# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-scene.nix — the 3D pass, the seam, and the 2D pass, in one
# frame, on the host.
#
# kiln-gui covers the 2D half. This covers the whole bracket:
#
#     kiln_frame_begin()      attach colour + Z
#       kiln_scene_begin()    3D: perspective, lit, depth-tested
#         ... geometry ...
#       kiln_gui_begin()      <- the seam: depth OFF
#         ... HUD ...
#       kiln_gui_end()
#     kiln_frame_end()        present
#
# which CLAUDE.md calls the one state transition per frame and the thing that
# makes drawing HUD inside the 3D pass an obvious mistake. Until this existed,
# nothing outside a ROM on real hardware had ever executed it.
#
# The scene is built from what a host 3D pass can actually get wrong: two cubes
# overlapping in screen space at different depths, six faces with known
# normals, two directional lights plus ambient, fog with the far cube inside
# its range, transforms through t3d_mat4_to_fixed's s16.16 quantisation, and a
# HUD line drawn straight across both cubes that must land on top.
#
# ── What it caught on its first run ───────────────────────────────────
# The cubes rendered inside-out: the far faces drew over the near ones. It
# looked exactly like a broken depth compare in the rasteriser. It was not —
# nothing in kiln_engine.c calls rdpq_mode_zbuf at all, because Tiny3D's own
# t3d_frame_start does it (t3d.c:176), and the host shim had not reproduced
# that. T3D_FLAG_DEPTH is the RSP's half of depth; rdpq_mode_zbuf is the RDP's.
# Both are needed, and only one of them is visible in engine code.
#
# ── What it does NOT prove ────────────────────────────────────────────
# That the console draws these pixels. There is no RDP here: no scissoring, no
# 16-bit colour dithering, no coverage-based antialiasing, and no near-plane
# CLIPPING (this rejects a triangle that straddles the eye plane where Tiny3D
# would trim it — see host_t3d.c). Above all, no fill rate, which is the
# console's actual binding constraint. `./dev shot` remains the arbiter of what
# a frame looks like; this holds the geometry, lighting and layout arithmetic
# still between changes.
{ pkgs, engineSrc, platHost, hostMath }:

pkgs.runCommand "check-kiln-scene"
{
  nativeBuildInputs = [ pkgs.gcc ];
  buildInputs = [ pkgs.zlib ];
  meta.description = "the full 3D + seam + 2D frame bracket renders byte-identically";
}
  ''
    set -euo pipefail

    gcc -O1 -g -std=gnu2x -Wall -Wextra -Werror \
        -I${platHost}/include -I${platHost}/src -I${hostMath}/include \
        -I${engineSrc}/src/kiln \
        -o scenecheck \
        ${./kiln-scene-check.c} \
        ${engineSrc}/src/kiln/kiln_engine.c \
        ${engineSrc}/src/kiln/kiln_gui.c \
        \
        $(echo ${platHost}/src/*.c) \
        ${hostMath}/lib/libkilnmath.a -lz -lm

    ./scenecheck out.png out.txt

    fail=0
    if ! cmp -s out.txt ${./refs/kiln-scene.txt}; then
      echo ""; echo "FAILED: the HUD text manifest changed."
      diff -u ${./refs/kiln-scene.txt} out.txt || true
      fail=1
    fi
    if ! cmp -s out.png ${./refs/kiln-scene.png}; then
      echo ""; echo "FAILED: the rendered frame changed."
      echo "  reference $(stat -c%s ${./refs/kiln-scene.png}) bytes, "\
           "rendered $(stat -c%s out.png) bytes"
      echo "The counters printed above say whether geometry or shading moved."
      echo "Magnify both before accepting a new reference — a gate is perfectly"
      echo "happy to freeze a wrong picture forever."
      fail=1
    fi
    [ $fail -eq 0 ] || exit 1

    echo "the frame bracket matches its reference capture"
    mkdir -p $out && cp out.png out.txt $out/
  ''
