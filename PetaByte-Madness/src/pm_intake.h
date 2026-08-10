// SPDX-License-Identifier: MPL-2.0
//
// pm_intake.h — the transformation.
//
// Horner climbs onto the scanner slab and the machine takes him in. It is
// the hinge of the whole game: everything before it is a man in a room,
// everything after it is the thing on the beach.
//
// ── The choreography is authored, not invented ─────────────────────────
// reference/ph_anim_intake.h is the animation as its generator emitted it:
// 218 frames at 15 fps, eight root keys, four slab keys, and five named
// cues. This module plays THAT, in its own units, rather than a
// re-timed approximation of it — see pm_intake.c on why the numbers are
// left in s16 and converted at runtime instead of pre-baked.
//
//     LOOK  frame   0   he is just standing there
//     SIT   frame  66   weight lands on the slab
//     LIE   frame 126   head touches down
//     MOTOR frame 144   slab drive starts
//     IN    frame 195   he is inside
//
// ── One trap, and the header warns about it ────────────────────────────
// From CUE_LIE onward the root translation is expressed in the SLAB's
// frame, so the slab's own travel has to be added to it. Miss that and the
// machine slides out from under him — he lies still while the bed leaves
// without him, which looks like a broken animation rather than a wrong
// parent.
//
// ── What Pass A does not do ────────────────────────────────────────────
// Horner is drawn as a RIGID body here: the root position and pitch are
// animated, the seventeen joint tracks are not. He tips from standing to
// flat in one piece rather than bending at the waist.
//
// The camera is cut to hide it — wide while he is upright, overhead once
// he is flat, then at the mouth of the bore — and at those distances a
// low-poly figure reads by silhouette and motion, which are both correct.
// It is still a stand-in. Pass B runs ph_rig.py through the same pipeline
// that gave the centaur its thirteen animations (docs/ASSET_PIPELINE.md),
// after which the pose keys play and the camera can go anywhere.

#ifndef PM_INTAKE_H
#define PM_INTAKE_H

#include "pm_demo.h"

/** The intake, as a shot the screen machine plays like any other. */
extern const PMDemoShot pm_intake_shot;

#endif // PM_INTAKE_H
