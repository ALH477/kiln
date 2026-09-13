# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-vanim.nix — kiln_morph's CPU blend.
#
# The blend scaled packed RGBA words by float weights and never wrote normals,
# so every morph on console garbled its colours and lit at flat ambient. The
# host cannot draw a morph (vertex placeholders abort), but the blend is plain
# arithmetic and is asserted here. See kiln-vanim-check.c.
{ pkgs, target }:

target.mkCheck {
  pname = "vanimcheck";
  sources = [ ./kiln-vanim-check.c ];
  meta.description = "kiln_morph blends colour per channel and normals between its two heaviest targets, rounds once, clamps weights and alternates buffers; kiln_deform starts from its base every frame";
}
