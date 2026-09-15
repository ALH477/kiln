# SPDX-License-Identifier: MIT
#
# fps's jump ROMs and host builds. See nix/demos/README.md.
#
#   fps-switch   the corridor switch pressed, the gate rising
#   fps-combat   the hall grunts in front of the player, the pistol firing
#   fps-door     at the arena's red door with the red key, A opens it
#
# Each pc-fps-<jump> is the same main.c with the same KILN_JUMP, so a host
# render and an Ares capture of one jump are of the same frame.
ctx: with ctx;
let
  fpsArgs = args.fpsArgs;
  jumps = [ "SWITCH" "COMBAT" "DOOR" ];
  pcGame = jump:
    let suffix = if jump == null then "" else "-" + lib.toLower jump;
    in lib.nameValuePair "pc-fps${suffix}" (hostNative.mkGame {
      pname = "kiln-fps${suffix}";
      sources = [ ../../examples/fps/main.c ];
      assets = fpsArgs.assets;
      extraCFlags = lib.optional (jump != null) "-DKILN_JUMP=JUMP_${jump}";
      meta.description = "fps${suffix}, on this machine";
    });
in
mkJumpRoms fpsArgs jumps
// lib.listToAttrs (map pcGame ([ null ] ++ jumps))
