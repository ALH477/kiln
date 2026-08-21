// SPDX-License-Identifier: MPL-2.0
//
// pm_debug.h — the state readout that would have caught all four of them.
//
// This exists because of a specific, expensive failure. The ROM rendered
// black and the reasons were: the veil was writing scene->far_z on every
// screen including the menus, the flyover camera sat inside the island's
// footprint and below its peak, the boot logo was quantised to three
// integer units by the wrong baseScale, and no ares input was bound so no
// screen past the splash was reachable at all. Not one of those is
// visible in a screenshot — a black frame looks the same whatever caused
// it — and three of them are numeric, so they cannot be confirmed by
// reading either. Each was found by instrumenting for it after the fact.
//
// So the overlay prints the numbers that distinguish them: which screen
// is up, where the camera is and what it is looking at, the near/far pair
// actually in the scene this frame, and which models resolved versus
// returned NULL. A glance answers "is the camera inside the geometry",
// "did the far plane get clamped", "is the mesh even loaded" — the three
// questions a black frame refuses to answer.
//
// ── Debug ROM only ─────────────────────────────────────────────────────
// Everything here compiles to nothing without KILN_DEBUG, which
// mkN64Rom's `debugConsole = true` defines (nix/rom.nix). flake.nix
// builds `petabyte-madness-debug` with it; the shipping ROM pays nothing,
// not even the text.

#ifndef PM_DEBUG_H
#define PM_DEBUG_H

#include <kiln/kiln_engine.h>
#include <kiln/kiln_fpscam.h>
#include <kiln/kiln_camera.h>

#include "pm_screens.h"
#include "pm_veil.h"

#ifdef KILN_DEBUG

/** Draw the readout. Call inside the GUI pass, after everything else, so
 *  it sits over whatever it is describing.
 *
 *  `scene` must be the scene AFTER the frame's camera has been applied and
 *  the veil (if any) has written to it — the whole point is to report what
 *  the projection was actually built from, not what it was initialised
 *  with. */
void pm_debug_draw(const PMApp *app, const KilnScene *scene,
                   const KilnFpsCam *fps, const PMVeil *veil,
                   float dt, int screen_w, int screen_h);

/** Toggle the overlay, and cycle the spatial layers. Bound to chords the
 *  game itself does not use. */
void pm_debug_input(const KilnInput *in);

/** Draw the SPATIAL overlay — the half that answers "where is it" rather
 *  than "what is it".
 *
 *  Everything the text readout above reports is a number, and every defect
 *  pm_debug.h's opening paragraph lists is a position: a camera inside the
 *  island's footprint, a shot opening behind a wall, a machine standing where
 *  nothing framed it. Numbers made those findable in retrospect; drawing them
 *  makes them findable at a glance. Layers, cycled with the chord below:
 *
 *    PM_DD_BRUSHES  the lab's hand-authored collision boxes (pm_lab.c) —
 *                   geometry that exists only as numbers in a C file
 *    PM_DD_ACTORS   every live actor's bounds and facing, plus its category
 *    PM_DD_CAMERA   the playing shot's keyframe path, with a sightline from
 *                   each eye key to its look target
 *    PM_DD_ANCHORS  the named world positions the intro is keyed to — the
 *                   desk, the MRI, the arms, the origin — as axis gizmos
 *
 *  Call inside the GUI pass, BEFORE pm_debug_draw so the text panel sits on
 *  top of the lines rather than under them. `scene` has the same contract as
 *  pm_debug_draw's: it must be the scene the frame was really built from. */
void pm_debug_draw3d(const PMApp *app, const KilnScene *scene,
                     int screen_w, int screen_h);

/** What the spatial overlay drew this frame, for the text panel to report:
 *  "dd 12/48" distinguishes "no layers on" from "the layer is on and every
 *  primitive is behind you", which look identical on screen. */
void pm_debug_dd_counts(uint16_t *drawn, uint16_t *clipped);

#else

#define pm_debug_draw(app, scene, fps, veil, dt, w, h) ((void)0)
#define pm_debug_input(in)                             ((void)0)
#define pm_debug_draw3d(app, scene, w, h)              ((void)0)
#define pm_debug_dd_counts(drawn, clipped)             ((void)0)

#endif // KILN_DEBUG

#endif // PM_DEBUG_H
