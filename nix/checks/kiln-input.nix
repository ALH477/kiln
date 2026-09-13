# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-input.nix — kiln_input's scripted-input tapes, frame by frame.
#
# Attract modes and jump ROMs are how every example is made to show something
# with no controller attached. When a tape is off by a frame or never cancels,
# the symptom is a demo standing still, which no capture can tell apart from a
# dozen other faults. See kiln-input-check.c.
{ pkgs, target }:

target.mkCheck {
  pname = "inputcheck";
  sources = [ ./kiln-input-check.c ];
  meta.description = "kiln_input tapes produce real edges, loop, force, and hand back to a real pad";
}
