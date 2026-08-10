// SPDX-License-Identifier: MPL-2.0
//
// pm_hud.h — the 2D pass. Immediate mode on m64_gui, same as every other
// M64 project: a dozen HUD rectangles on a 93.75 MHz VR4300 do not want a
// retained widget tree.
//
// The HUD's whole job is to make the veil's *cost* legible, because the
// cost is the design (docs/VEIL_DESIGN.md §6). It shows three things:
// air (which the veil burns), whether something is currently looking at
// you, and whether your toggle is still yours or an overlord has taken it.

#ifndef PM_HUD_H
#define PM_HUD_H

#include <m64/m64_engine.h>

#include "pm_types.h"
#include "pm_veil.h"

/** Draw the HUD. Call inside the 2D pass, between m64_gui_begin and
 *  m64_gui_end. */
void pm_hud_draw(const PMPlayer *pl, const PMVeil *veil, int screen_w,
                 int screen_h);

#endif // PM_HUD_H
