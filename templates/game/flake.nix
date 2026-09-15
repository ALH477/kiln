# SPDX-License-Identifier: MIT
#
# A Kiln game. Created with `nix flake init -t github:ALH477/kiln#game`, or
# `./dev new <name>` from a Kiln checkout (which also renames `mygame`).
#
#   nix build                      the ROM, result/mygame.z64
#   nix run kiln#ares -- result/mygame.z64
#   nix eval --json .#studioManifest.x86_64-linux   what Kiln Studio reads
#
# The ROM builds only on Linux (the N64 cross toolchain is Linux-only).
{
  description = "mygame — a game on the Kiln engine";

  inputs.kiln.url = "github:ALH477/kiln";

  outputs = { self, kiln }:
    let
      system = "x86_64-linux";
      game = import ./game.nix { inherit kiln system; };
    in
    {
      packages.${system} = game // { default = game.rom; };

      # Kiln Studio's project model for this repository: the ROM above, and
      # any jump ROMs or host builds added to game.nix later.
      studioManifest.${system} = kiln.lib.${system}.mkStudioManifest {
        inherit system;
        packages = self.packages.${system};
      };
    };
}
