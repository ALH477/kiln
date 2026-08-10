// SPDX-License-Identifier: MPL-2.0
//
// pm_hud.c — see pm_hud.h.

#include "pm_hud.h"

#include <libdragon.h>
#include <m64/m64_gui.h>

void pm_hud_draw(const PMPlayer *pl, const PMVeil *veil, int screen_w,
                 int screen_h)
{
    const color_t ink    = RGBA32(0xD8, 0xD2, 0xC8, 0xFF);
    const color_t warn   = RGBA32(0xE0, 0x2A, 0x28, 0xFF);
    const color_t fill   = RGBA32(0x08, 0x0A, 0x0E, 0xC0);
    const color_t border = RGBA32(0x3A, 0x40, 0x48, 0xFF);

    // ── Health + air, bottom left ──────────────────────────────────────
    m64_gui_panel(6, screen_h - 40, 108, 34, fill, border);
    m64_gui_text(12, screen_h - 28, ink, "HP");
    m64_gui_bar(30, screen_h - 34, 76, 8,
                (float)pl->health / (float)pl->max_health,
                RGBA32(0xC8, 0x30, 0x2C, 0xFF), RGBA32(0x28, 0x14, 0x14, 0xFF));

    // Air is the veil's meter as much as the diver's: it drains faster
    // while the filter is up, which is the whole reason the player ever
    // turns it off.
    m64_gui_text(12, screen_h - 12, ink, "AIR");
    m64_gui_bar(30, screen_h - 18, 76, 8, pl->air / pl->max_air,
                pl->air < pl->max_air * 0.25f
                    ? warn : RGBA32(0x3C, 0x9E, 0xC0, 0xFF),
                RGBA32(0x12, 0x1C, 0x24, 0xFF));

    // ── Veil state, bottom right ───────────────────────────────────────
    // The step, not the continuous t: the player is managing a filter
    // wheel with nine detents, and the readout should agree with what
    // their eyes are being shown.
    m64_gui_panel(screen_w - 96, screen_h - 40, 90, 34, fill, border);
    m64_gui_text(screen_w - 90, screen_h - 28,
                 veil->forced ? warn : ink,
                 veil->forced ? "VEIL FORCED" : "VEIL");
    m64_gui_bar(screen_w - 90, screen_h - 18, 78, 8,
                (float)veil->step / (float)(PM_VEIL_STEPS - 1),
                RGBA32(0x8E, 0x0C, 0x12, 0xFF), RGBA32(0x1A, 0x10, 0x12, 0xFF));

    // ── Reciprocity ────────────────────────────────────────────────────
    // The one line of text that explains the whole mechanic to a player
    // who has not read the design doc: raising the filter is not free,
    // because while it is up they can see you too.
    if (pl->seen) {
        m64_gui_text(screen_w / 2 - 26, 22, warn, "SEEN");
    }

    // Crosshair. Two ticks, not a cross — a solid dot competes with the
    // eye pinpricks, which are the only thing on screen worth looking at
    // with the veil down.
    const int cx = screen_w / 2, cy = screen_h / 2;
    m64_gui_rect(cx - 4, cy, 3, 1, ink);
    m64_gui_rect(cx + 2, cy, 3, 1, ink);
}
