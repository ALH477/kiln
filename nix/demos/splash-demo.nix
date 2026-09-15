# SPDX-License-Identifier: MIT
#
# splash-demo: the boot splash, then a lit turntable of the kiln logo.
#   splash-demo-stage     boots onto the turntable and never auto-replays
#   pc-splash-demo[-stage] the same main.c on this machine (kiln_splash and
#                          rigid models both run on the host renderer)
ctx: with ctx;
let a = args.splashDemoArgs; in
mkJumpRoms a [ "STAGE" ] // {
  pc-splash-demo = hostNative.mkGame {
    pname = "kiln-splash-demo";
    sources = [ ../../examples/splash-demo/main.c ];
    assets = a.assets;
    meta.description = "splash-demo, on this machine";
  };
  pc-splash-demo-stage = hostNative.mkGame {
    pname = "kiln-splash-demo-stage";
    sources = [ ../../examples/splash-demo/main.c ];
    assets = a.assets;
    extraCFlags = [ "-DKILN_JUMP=JUMP_STAGE" ];
    meta.description = "splash-demo-stage, on this machine";
  };
}
