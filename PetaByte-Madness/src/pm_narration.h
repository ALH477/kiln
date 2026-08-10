// SPDX-License-Identifier: MPL-2.0
//
// pm_narration.h — the backstory, before any of it is playable.
//
// The first thing a new profile sees after picking an empty save slot:
// fade to black, the crawl below, then LAB_CINE picks up from there. It is
// a PMDemoShot like LAB_CINE/INTAKE/SUB/BEACH — pm_screens.c starts it,
// waits on pm_demo_done(), and moves on — but it draws nothing in the 3D
// pass (there is nothing to show yet) and everything in the 2D pass, so it
// exposes pm_narration_draw2d the same way pm_lab.c exposes pm_lab_draw2d
// for a screen that isn't purely shot-driven.
//
// ── Tolling, not typed ──────────────────────────────────────────────────
// ostafterstart.mp3 (254.13 s) is the score. This crawl is sized to it —
// see pm_narration.c's TYPE_RATE comment — rather than to whatever pace a
// dialogue box would use: a bell tolls slowly, and five short pages over
// four minutes is what that reads like on screen. START skips it, same as
// every other beat in the intro.

#ifndef PM_NARRATION_H
#define PM_NARRATION_H

#include "pm_demo.h"

/** The narration crawl, as a shot the screen machine plays like any other. */
extern const PMDemoShot pm_narration_shot;

/** Draw the current page's revealed lines. Call inside the 2D/GUI pass —
 *  there is no 3D content for this shot, so pm_screens.c calls this
 *  directly rather than through pm_demo_draw(). */
void pm_narration_draw2d(int screen_w, int screen_h);

#endif // PM_NARRATION_H
