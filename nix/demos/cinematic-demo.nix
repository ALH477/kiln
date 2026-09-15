# SPDX-License-Identifier: MIT
#
# examples/cinematic-demo's jump ROMs. See nix/demos/README.md.
#
#   cinematic-demo-t10 / -t22 / -t30 / -t33 / -t38
#       the timeline fast-forwarded to that second and held there (the cast
#       keeps animating in place), so a capture does not depend on how long
#       Ares took to boot: the crates, the bay door and its alarm, the
#       stand-off, the captain's taunt with its subtitle, the wide
#   cinematic-demo-boxes
#       the running loop with every actor's measured bounds drawn over it
#
# No pc-* build: every actor is a skinned model, and the host aborts in
# t3d_skeleton_create.
ctx: with ctx;
mkJumpRoms args.cinematicDemoArgs [ "T10" "T22" "T30" "T33" "T38" "BOXES" ]
