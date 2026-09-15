# SPDX-License-Identifier: MIT
#
# The game's packages, as a function of Kiln, so the same file is built by this
# repository's flake.nix and by Kiln's own checks.template-game (which is what
# keeps the template from rotting).
#
# Add assets with kiln.lib.${system}.mkModel / mkSound / mkTextures / … and pass
# them in `assets`; add jump ROMs by building the same args with
# makeFlags = [ "KILN_JUMP=NAME" ]. See Kiln's CLAUDE.md.
{ kiln, system }:

let
  k = kiln.lib.${system};
in
{
  rom = k.mkN64Rom {
    name = "mygame";
    src = ./mygame;
    romTitle = "My Game";
  };
}
