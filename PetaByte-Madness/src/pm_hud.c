// SPDX-License-Identifier: MPL-2.0
//
// pm_hud.c — see pm_hud.h.

#include "pm_hud.h"

#include <libdragon.h>
#include <kiln/kiln_gui.h>

// Panel alpha at a bar's safe value (full HP/air, veil at rest) vs. its
// alert value. Nothing is ever hidden — every bar and label is always
// drawn — but a panel that has nothing to say quiets itself down instead
// of sitting at full opacity all game, which is most of what "minimal"
// means for a HUD that already shows the least it can. `t` is 0 (safe) to
// 1 (alert); the floor keeps the panel legible rather than invisible.
static uint8_t hud_alpha(float t)
{
    const float floor = 0.35f, a = floor + (1.0f - floor) * t;
    return (uint8_t)(a * 255.0f);
}

static color_t panel_at(float t)
{
    color_t c = PM_UI_PANEL;
    c.a = hud_alpha(t);
    return c;
}

static color_t border_at(float t)
{
    color_t c = PM_UI_BORDER;
    c.a = hud_alpha(t);
    return c;
}

void pm_hud_draw(const PMPlayer *pl, const PMVeil *veil, int screen_w,
                 int screen_h)
{
    const color_t ink  = PM_UI_INK;
    const color_t warn = PM_UI_WARN;

    // ── Health + air, bottom left, one panel ───────────────────────────
    // Whichever of the two is furthest from safe drives the panel's own
    // alpha — a calm HUD when both are fine, legible chrome the moment
    // either one genuinely needs attention.
    const float hp_t  = 1.0f - (float)pl->health / (float)pl->max_health;
    const float air_t = 1.0f - pl->air / pl->max_air;
    const float left_t = hp_t > air_t ? hp_t : air_t;
    kiln_gui_panel(6, screen_h - 40, 108, 34, panel_at(left_t), border_at(left_t));
    kiln_gui_text(12, screen_h - 28, ink, "HP");
    kiln_gui_bar(30, screen_h - 34, 76, 8,
                (float)pl->health / (float)pl->max_health,
                RGBA32(0xC8, 0x30, 0x2C, 0xFF), RGBA32(0x28, 0x14, 0x14, 0xFF));

    // Air is the veil's meter as much as the diver's: it drains faster
    // while the filter is up, which is the whole reason the player ever
    // turns it off.
    kiln_gui_text(12, screen_h - 12, ink, "AIR");
    kiln_gui_bar(30, screen_h - 18, 76, 8, pl->air / pl->max_air,
                pl->air < pl->max_air * 0.25f
                    ? warn : RGBA32(0x3C, 0x9E, 0xC0, 0xFF),
                RGBA32(0x12, 0x1C, 0x24, 0xFF));

    // ── Veil state, bottom right ───────────────────────────────────────
    // The step, not the continuous t: the player is managing a filter
    // wheel with nine detents, and the readout should agree with what
    // their eyes are being shown.
    const float veil_t = (float)veil->step / (float)(PM_VEIL_STEPS - 1);
    kiln_gui_panel(screen_w - 96, screen_h - 40, 90, 34,
                  panel_at(veil_t), border_at(veil_t));
    kiln_gui_text(screen_w - 90, screen_h - 28,
                 veil->forced ? warn : ink,
                 veil->forced ? "VEIL FORCED" : "VEIL");
    kiln_gui_bar(screen_w - 90, screen_h - 18, 78, 8, veil_t,
                RGBA32(0x8E, 0x0C, 0x12, 0xFF), RGBA32(0x1A, 0x10, 0x12, 0xFF));

    // ── Reciprocity ────────────────────────────────────────────────────
    // The one line of text that explains the whole mechanic to a player
    // who has not read the design doc: raising the filter is not free,
    // because while it is up they can see you too.
    if (pl->seen) {
        kiln_gui_text(screen_w / 2 - 26, 22, warn, "SEEN");
    }

    // Crosshair. Two ticks, not a cross — a solid dot competes with the
    // eye pinpricks, which are the only thing on screen worth looking at
    // with the veil down.
    const int cx = screen_w / 2, cy = screen_h / 2;
    kiln_gui_rect(cx - 4, cy, 3, 1, ink);
    kiln_gui_rect(cx + 2, cy, 3, 1, ink);
}
