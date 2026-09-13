# SPDX-License-Identifier: MIT
#
# examples/cinematic-demo's jump ROMs. See nix/demos/README.md.
#
#   cinematic-demo-boxes   the loop with every actor's measured bounds drawn
ctx: with ctx;
mkJumpRoms args.cinematicDemoArgs [ "BOXES" ]
