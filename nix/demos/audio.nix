# SPDX-License-Identifier: MIT
#
# examples/audio: the baked pluck as an instrument. No jump ROMs — the demo
# plays itself from boot (sequencer) and its one input-reached state, a pluck,
# is a moment rather than a place. The host build decodes the same wav64 and
# draws the same frame, scope included.
ctx: with ctx; {
  pc-audio = hostNative.mkGame {
    pname = "kiln-audio";
    sources = [ ../../examples/audio/audio.c ];
    assets = args.audioArgs.assets;
  };
}
