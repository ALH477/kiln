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
// LOOK's own beat (frames 0-45: stands still, then turns to face it) is now
// covered by the new leading sequence's own arrival instead — the new
// sequence hands off at this file's original frame 45, already turned to
// face the machine, so CUE_SIT/CUE_LIE/CUE_MOTOR/CUE_IN above are unchanged.
//
// ── One trap, and the header warns about it ────────────────────────────
// From CUE_LIE onward the root translation is expressed in the SLAB's
// frame, so the slab's own travel has to be added to it. Miss that and the
// machine slides out from under him — he lies still while the bed leaves
// without him, which looks like a broken animation rather than a wrong
// parent.
//
// ── Pass B is live ──────────────────────────────────────────────────────
// Horner is a skinned KilnSkel now (tools/blender/horner.py, built from
// ph_rig.py + ph_anim_clips.py via ph_rig_export.py — see that file for the
// coordinate/Euler derivation). The ROOT position and pitch below are still
// animated exactly as Pass A left them — a skinned mesh needs its overall
// placement driven the same way a rigid one did — but the joint bends
// (sitting, reclining, lying) now come from the `climb_in` clip instead of
// tipping the whole rigid body over in one piece.
//
// Ahead of this file's original beats (which now start at ORIG_HANDOFF_FRAME
// rather than frame 0), a new leading sequence plays out the same way
// pm_arrival.c drives the centaur: named clips switched on state changes,
// gated by kiln_skel_is_done() for one-shots, with the ROOT path a plain
// per-state position lerp rather than a transcribed spline — there is no
// mocap to transcribe for "walks to a console and sits down", unlike the
// scanner sequence below.

#ifndef PM_INTAKE_H
#define PM_INTAKE_H

#include "pm_demo.h"

/** The intake, as a shot the screen machine plays like any other. */
extern const PMDemoShot pm_intake_shot;

#endif // PM_INTAKE_H
