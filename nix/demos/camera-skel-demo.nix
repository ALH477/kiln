# SPDX-License-Identifier: MIT
#
# camera-skel-demo: the goblin walking a courtyard behind a spring-arm camera.
#   camera-skel-demo-walk  the figure-8 tape, forced from boot
#   camera-skel-demo-rig   the hand-built 2-bone rig (skelModel) in place of
#                          the goblin, walking a circle
#
# No pc-* build: t3d_skeleton_* and t3d_anim_* abort on the host, and this demo
# is skinned in every mode. It is verified by `nix build` and Ares captures.
ctx: with ctx;
mkJumpRoms args.cameraSkelDemoArgs [ "WALK" "RIG" ]
