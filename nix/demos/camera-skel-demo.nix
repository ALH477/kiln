# SPDX-License-Identifier: MIT
#
# camera-skel-demo: the goblin in a courtyard behind a spring-arm camera —
# locomotion over kiln_skel's slots, jumps, a masked sword overlay, a socketed
# sword and an inspector.
#   camera-skel-demo-walk  the attract tape, forced from boot
#   camera-skel-demo-run   a flat-out circle: Walk/Run slots, stride-synced
#   camera-skel-demo-jump  running and jumping (Jump, Fall, Land)
#   camera-skel-demo-atk   sword swings over a walk: the torso-masked overlay
#   camera-skel-demo-insp  the attract tape with the inspector open
#   camera-skel-demo-rig   the hand-built 2-bone rig (skelModel) in place of
#                          the goblin, walking a circle
#
# No pc-* build: t3d_skeleton_* and t3d_anim_* abort on the host, and this demo
# is skinned in every mode. It is verified by `nix build` and Ares captures.
ctx: with ctx;
mkJumpRoms args.cameraSkelDemoArgs [ "WALK" "RUN" "JUMP" "ATK" "INSP" "RIG" ]
