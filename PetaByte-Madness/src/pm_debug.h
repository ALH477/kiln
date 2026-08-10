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
// Everything here compiles to nothing without M64_DEBUG, which
// mkN64Rom's `debugConsole = true` defines (nix/rom.nix). flake.nix
// builds `petabyte-madness-debug` with it; the shipping ROM pays nothing,
// not even the text.

#ifndef PM_DEBUG_H
#define PM_DEBUG_H

#include <m64/m64_engine.h>
#include <m64/m64_fpscam.h>
#include <m64/m64_camera.h>

#include "pm_screens.h"
#include "pm_veil.h"

#ifdef M64_DEBUG

/** Draw the readout. Call inside the GUI pass, after everything else, so
 *  it sits over whatever it is describing.
 *
 *  `scene` must be the scene AFTER the frame's camera has been applied and
 *  the veil (if any) has written to it — the whole point is to report what
 *  the projection was actually built from, not what it was initialised
 *  with. */
void pm_debug_draw(const PMApp *app, const M64Scene *scene,
                   const M64FpsCam *fps, const PMVeil *veil,
                   float dt, int screen_w, int screen_h);

/** Toggle the overlay. Bound to a chord the game itself does not use. */
void pm_debug_input(const M64Input *in);

#else

#define pm_debug_draw(app, scene, fps, veil, dt, w, h) ((void)0)
#define pm_debug_input(in)                             ((void)0)

#endif // M64_DEBUG

#endif // PM_DEBUG_H
