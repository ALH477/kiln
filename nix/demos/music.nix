# SPDX-License-Identifier: MIT
#
# examples/music. Two jump ROMs, one per track, both with the attract tape off:
# music-xm boots on the XM module, music-cine on the looping wav64 bed. The base
# ROM's tape changes track after 4 s idle, so it cannot be captured twice on
# the same track.
#
# The host builds are real for the bed (the wav64 decodes and mixes, so its
# scope and meters move) and bookkeeping for the XM module: plat/host's XM64
# player reserves channels and reports position 0, and plays no notes. Judge
# track 1 in Ares, never here.
ctx: with ctx;
mkJumpRoms args.musicDemoArgs [ "CINE" "XM" ] // {
  pc-music = hostNative.mkGame {
    pname = "kiln-music";
    sources = [ ../../examples/music/main.c ];
    assets = args.musicDemoArgs.assets;
  };
  pc-music-cine = hostNative.mkGame {
    pname = "kiln-music-cine";
    sources = [ ../../examples/music/main.c ];
    assets = args.musicDemoArgs.assets;
    extraCFlags = [ "-DKILN_JUMP=JUMP_CINE" ];
  };
}
