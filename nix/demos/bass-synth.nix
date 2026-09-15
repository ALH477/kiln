# SPDX-License-Identifier: MIT
#
# examples/bass-synth. The jump ROM boots straight into the patch editor with
# the intro skipped and no attract tape, so a capture shows the menu and the
# save backend line with no pad.
#
# Built with mkN64Rom rather than mkJumpRoms because "Kiln Bass Synth PATCH"
# is 21 characters and the ROM header holds 20.
ctx: with ctx; {
  bass-synth-patch = mkN64Rom (args.bassSynthArgs // {
    name = "bass-synth-patch";
    romTitle = "Kiln Bass PATCH";
    makeFlags = (args.bassSynthArgs.makeFlags or [ ]) ++ [ "KILN_JUMP=PATCH" ];
  });

  # The host has a real mixer for wav64, so the synth sounds on PC; kiln_store
  # falls through to read-only rom:/ there, which the HUD shows in red.
  pc-bass-synth = hostNative.mkGame {
    pname = "kiln-bass-synth";
    sources = [ ../../examples/bass-synth/main.c ];
    assets = args.bassSynthArgs.assets;
  };
  pc-bass-synth-patch = hostNative.mkGame {
    pname = "kiln-bass-synth-patch";
    sources = [ ../../examples/bass-synth/main.c ];
    assets = args.bassSynthArgs.assets;
    extraCFlags = [ "-DKILN_JUMP=JUMP_PATCH" ];
  };
}
