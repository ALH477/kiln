// SPDX-License-Identifier: MPL-2.0
//
// pm_hud.h — the 2D pass. Immediate mode on kiln_gui, same as every other
// Kiln project: a dozen HUD rectangles on a 93.75 MHz VR4300 do not want a
// retained widget tree.
//
// The HUD's whole job is to make the veil's *cost* legible, because the
// cost is the design (docs/VEIL_DESIGN.md §6). It shows three things:
// air (which the veil burns), whether something is currently looking at
// you, and whether your toggle is still yours or an overlord has taken it.

#ifndef PM_HUD_H
#define PM_HUD_H

#include <kiln/kiln_engine.h>

#include "pm_types.h"
#include "pm_veil.h"

// ── The house palette ────────────────────────────────────────────────────
// One set of colors for every screen's 2D chrome — the HUD, the lab's
// interaction prompt, and (via a matching KilnWidgetStyle built from these
// in pm_screens.c) the file-select and title menus — instead of three
// files each hand-typing their own close-but-not-quite panel/border.
// These are exactly the values pm_hud.c already used; naming and sharing
// them, not inventing a new look.
//
// kiln_dialogue's cyan is deliberately NOT folded in here: it is an
// engine-level default with no style-override parameter
// (engine/src/kiln/kiln_dialogue.c), used by any game that includes the
// module, and recoloring it is out of scope for a PM-only visual pass.
#define PM_UI_INK    RGBA32(0xD8, 0xD2, 0xC8, 0xFF)
#define PM_UI_PANEL  RGBA32(0x08, 0x0A, 0x0E, 0xC0)
#define PM_UI_BORDER RGBA32(0x3A, 0x40, 0x48, 0xFF)
#define PM_UI_WARN   RGBA32(0xE0, 0x2A, 0x28, 0xFF)
// The two menu-only additions KilnWidgetStyle needs (kiln_widget.h) that the
// HUD itself has no use for: a dimmed ink for unselected rows, and an
// accent for the selected one. The air bar's "safe" blue is already this
// game's one non-red accent, so the menu selection reuses it rather than
// introducing a second one.
#define PM_UI_DIM    RGBA32(0x80, 0x7A, 0x70, 0xFF)
#define PM_UI_ACCENT RGBA32(0x3C, 0x9E, 0xC0, 0xFF)

/** Draw the HUD. Call inside the 2D pass, between kiln_gui_begin and
 *  kiln_gui_end. */
void pm_hud_draw(const PMPlayer *pl, const PMVeil *veil, int screen_w,
                 int screen_h);

#endif // PM_HUD_H
