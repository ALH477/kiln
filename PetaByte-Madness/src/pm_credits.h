// SPDX-License-Identifier: MPL-2.0
//
// pm_credits.h — the title card, between the transformation and the beach.
//
// Fades in right after INTAKE's climb-in flash, plays the FMV (kiln_video.h)
// if one is present, then holds on a static title+credits card before
// handing off into the (unchanged) submarine scene. Like pm_narration.c,
// this shot draws nothing in the 3D pass and everything in the 2D pass, so
// pm_screens.c calls pm_credits_draw2d directly rather than through
// pm_demo_draw().
//
// ── Silent for now, and that is fine ────────────────────────────────────
// intro.m1v has no audio track yet — its intended companion (audio.wav) was
// reassigned to the surgery OST (see pm_lab.c). The FMV plays silent until
// real matched audio exists; dropping one in later is an asset swap, not a
// code change (kiln_video.h has no audio path at all — see its own header).

#ifndef PM_CREDITS_H
#define PM_CREDITS_H

#include "pm_demo.h"

/** The title/credits/video screen, as a shot the screen machine plays like
 *  any other. */
extern const PMDemoShot pm_credits_shot;

/** Draw the video frame (while playing) or the static title+credits card
 *  (once it ends). Call inside the 2D/GUI pass. */
void pm_credits_draw2d(int screen_w, int screen_h);

#endif // PM_CREDITS_H
