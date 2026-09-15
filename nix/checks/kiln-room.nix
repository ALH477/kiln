# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-room.nix — room streaming follows the camera.
#
# The loaded set used to be a closure that grew until it asserted, and the one
# demo of it could not leave its first room, so the defect was invisible. This
# walks a camera across a 2x2 grid and checks the set on every frame.
{ pkgs, target }:

target.mkCheck {
  pname = "roomcheck";
  sources = [ ./kiln-room-check.c ];
  meta.description = "kiln_room loads the camera's rooms and their neighbours, unloads the rest, and respects max_loaded";
}
