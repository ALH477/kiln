# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-fpscam.nix — the first-person camera turns and strafes the way
# the renderer draws.
#
# kiln_fpscam's "right" was the renderer's screen-left, so stick right strafed
# left and C right turned left in every consumer (examples/fps, Forge's WALK).
# This asks t3d_viewport_look_at where screen-right is and holds the pad to it.
# See kiln-fpscam-check.c.
{ pkgs, target }:

target.mkCheck {
  pname = "fpscamcheck";
  sources = [ ./kiln-fpscam-check.c ];
  meta.description = "kiln_fpscam: stick right strafes and C right turns toward the renderer's screen-right";
}
