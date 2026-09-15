# SPDX-License-Identifier: MIT
#
# nightlight-demo: looping nightlight. No jump ROMs — boot is the loop.
ctx: with ctx;
{
  pc-nightlight-demo = hostNative.mkGame {
    pname = "kiln-nightlight-demo";
    sources = [ ../../examples/nightlight-demo/main.c ];
    assets = args.nightlightDemoArgs.assets;
    meta.description = "nightlight-demo, on this machine";
  };
}
