# SPDX-License-Identifier: MIT
#
# examples/streamdb-demo: the FOCUS jump ROM, and host builds of both. The host
# reads the same assets.streamdb through the same kiln_asset + streamdb-embedded
# DFS backend, over a directory.
ctx: with ctx;
mkJumpRoms args.streamdbDemoArgs [ "FOCUS" ] // {
  pc-streamdb-demo = hostNative.mkGame {
    pname = "kiln-streamdb-demo";
    sources = [ ../../examples/streamdb-demo/main.c ];
    assets = args.streamdbDemoArgs.assets;
  };
  pc-streamdb-demo-focus = hostNative.mkGame {
    pname = "kiln-streamdb-demo-focus";
    sources = [ ../../examples/streamdb-demo/main.c ];
    assets = args.streamdbDemoArgs.assets;
    extraCFlags = [ "-DKILN_JUMP=JUMP_FOCUS" ];
  };
}
