# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-texrect.nix — rdpq_sprite_upload and rdpq_texture_rectangle on
# the host: scale, combiner, alpha compare and TMEM, pixel by pixel. They are
# what lets a ROM that draws a sprite build for the host at all. See
# kiln-texrect-check.c.
{ pkgs, target }:

target.mkCheck {
  pname = "texrectcheck";
  sources = [ ./kiln-texrect-check.c ];
  meta.description = "host sprite upload and texture rectangles sample, scale and combine as the RDP does";
}
