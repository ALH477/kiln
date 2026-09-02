# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-model.nix — a real .t3dm, parsed and rendered on the host.
#
# The model file is not committed: the check runs the SAME gltf_to_t3d the ROM
# build uses, over the same assets/cube.gltf, so the bytes under test are the
# bytes a ROM would link. A committed .t3dm would go stale the first time the
# converter changed, and going stale silently is the failure mode this whole
# directory exists to prevent.
#
# ── Why the structural assertions are the important half ──────────────
# plat/host/src/host_t3dmodel.c is the first part of the host backend that
# reimplements a FILE FORMAT rather than an API — Tiny3D's loader relocates the
# file in place, which a 64-bit host cannot do. A misread offset in a format
# reader does not fail; it produces geometry. So the check asserts the shape
# against what the converter reported (24 vertices, one object, one part, 12
# triangles from 24 strip indices) rather than only diffing pixels.
#
# ── Two things it caught that a render alone would not ────────────────
# 1. gltf_to_t3d emits triangle STRIPS. The cube has numIndices 0 and
#    numStripIndices[0] = 24 — six groups of four, each a quad. A reader
#    handling only t3d_tri_draw would load the file perfectly and draw nothing.
#
# 2. .t3dm is BIG-ENDIAN, because it is built for MIPS. Strip index 23 is
#    0x0017 and reads as 5888 little-endian, which trips the vertex-cache
#    assert immediately — the easy half. Vertex positions have the same problem
#    and no assert can catch them: -32 reads as -8193, so the model renders as
#    a spray of triangles that looks like a bad matrix. The reader converts
#    both explicitly.
#
# 3. And the one that needed real file data: t3d_vert_pack_normal's 5.6.5
#    fields are SIGNED two's complement scaled by 15.5/31.5/15.5, not an
#    unsigned mapping of [-1,1]. An unsigned pack paired with its own matching
#    unpack is self-consistent — a hand-built cube lights perfectly — and then
#    a real model's normals come out as nonsense. The cube's +Z face carries
#    0x000f, which only means (0,0,+1) under the signed reading.
#
# ── One body, every architecture ───────────────────────────────────────
# Built by nix/host.nix's `target`: it supplies the compiler, the flags and
# the three archives, so this file says what to render and what to compare
# and nothing about how to compile it. The same body runs under wasm32
# against the SAME reference files — flake.nix declares that variant as
# `<name>-wasm32`; `./dev arch <target>` runs it on the others.
{ pkgs, target, n64Inst, cubeGltf }:

target.mkCheck {
  pname = "modelcheck";
  sources = [ ./kiln-model-check.c ];
  args = "cube.t3dm out.png";
  meta.description = "a real .t3dm parses and renders on the host";
  # The same converter the ROM build uses, on the same source asset. It is a
  # build-machine tool, so it runs natively whatever the target is — which is
  # the point: one .t3dm, parsed by four architectures.
  preRun = ''
    ${n64Inst}/bin/gltf_to_t3d --ignore-materials ${cubeGltf} cube.t3dm
    echo "converted $(stat -c%s cube.t3dm) bytes of .t3dm"
  '';
  script = ''
    if ! cmp -s out.png ${./refs/kiln-model.png}; then
      echo ""
      echo "FAILED: the rendered model changed."
      echo "  reference $(stat -c%s ${./refs/kiln-model.png}) bytes, rendered $(stat -c%s out.png) bytes"
      echo "The structural assertions above passed, so the file parsed the same"
      echo "way — this is shading, projection or the strip winding. Magnify"
      echo "before accepting a new reference."
      exit 1
    fi
    echo "the model matches its reference capture (${target.description})"
  '';
}
