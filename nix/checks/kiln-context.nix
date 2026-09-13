# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-context.nix — the A-button context scan maps each actor to its action.
#
# Its door branch never ran (it compared a table index to a category id), so
# locked doors said "Open". See kiln-context-check.c.
{ pkgs, target }:

target.mkCheck {
  pname = "contextcheck";
  sources = [ ./kiln-context-check.c ];
  meta.description = "kiln_context returns UNLOCK for locked doors, OPEN, OPEN_CHEST, TALK and NONE where it should";
}
