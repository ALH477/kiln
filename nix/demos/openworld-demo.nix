# SPDX-License-Identifier: MIT
#
# openworld-demo: the FAST jump ROM and host builds. See nix/demos/README.md.
#
# FAST flies the autopilot at 3.5x so the stream pacer falls behind: proxy
# blocks on the near tiles and the pending gauge in the red, on a still frame.
ctx: with ctx;
let
  a = args.openworldDemoArgs;
  pc = jump: {
    name = "pc-openworld-demo${lib.optionalString (jump != null) "-${lib.toLower jump}"}";
    value = hostNative.mkGame {
      pname = "kiln-openworld-demo${lib.optionalString (jump != null) "-${lib.toLower jump}"}";
      sources = [ ../../examples/openworld-demo/main.c ];
      assets = a.assets;
      extraCFlags = lib.optional (jump != null) "-DKILN_JUMP=JUMP_${jump}";
      meta.description = "openworld-demo${lib.optionalString (jump != null) " (${jump})"}, on this machine";
    };
  };
in
mkJumpRoms a [ "FAST" ]
// lib.listToAttrs (map pc [ null "FAST" ])
