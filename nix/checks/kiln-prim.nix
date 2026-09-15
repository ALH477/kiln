# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-prim.nix — kiln_prim's geometry, read back rather than looked at.
#
# The winding assertion is the one worth the check: the engine sets no cull flag,
# so an inside-out face renders exactly like a correct one, and the defect would
# surface only on the day somebody turns culling on and half of every example's
# boxes disappear. See kiln-prim-check.c for the rest.
{ pkgs, target }:

target.mkCheck {
  pname = "primcheck";
  sources = [ ./kiln-prim-check.c ];
  meta.description = "kiln_prim boxes and floors wind outward, unpack to their axes and batch under the vertex cache";
}
