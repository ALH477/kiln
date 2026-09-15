# SPDX-License-Identifier: MIT
#
# texanim-demo's jump ROMs: each boots focused on one exhibit with its effect
# on, and stays there, so `./dev shot` can capture it with no controller.
#
# No pc-* build: every exhibit here is something the host cannot draw — tile
# callbacks do nothing there, and vertex FX and vertex placeholders abort.
ctx: with ctx;
mkJumpRoms args.texanimDemoArgs [ "SCROLL" "ENVMAP" "CEL" "FLAG" "MORPH" "FLIP" "PAL" "OFFSCR" ]
