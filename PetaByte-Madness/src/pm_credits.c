// SPDX-License-Identifier: MPL-2.0
//
// pm_credits.c — see pm_credits.h.

#include "pm_credits.h"

#include <libdragon.h>

#include <m64/m64_engine.h>
#include <m64/m64_gui.h>
#include <m64/m64_video.h>

#include "pm_hud.h"  // PM_UI_*

#define VIDEO_PATH "rom:/videos/intro.m1v"

// Sized generously rather than exactly: a raw .m1v elementary stream
// carries no reliable container duration (ffprobe's read on intro.m1v was
// itself untrustworthy for this reason — see the plan this shipped from),
// so this is a provisional ceiling meant to comfortably contain the clip
// plus a few seconds' hold on the static card. m64_video_update reports
// end-of-stream on its own regardless of this number — a shorter clip just
// means a longer hold, never a cut mid-playback. Tune down once the real
// playable length is known from a build.
#define SHOT_DURATION 60.0f

enum { PHASE_VIDEO, PHASE_HOLD };

static M64Video g_video;
static int      g_phase;
static float    g_hold_t;

static void credits_setup(void)
{
    m64_video_open(&g_video, VIDEO_PATH);
    g_phase = g_video.ready ? PHASE_VIDEO : PHASE_HOLD;
    g_hold_t = 0.0f;
    if (!g_video.ready) debugf("pm_credits: no %s, card only\n", VIDEO_PATH);
}

static void credits_teardown(void)
{
    m64_video_close(&g_video);
}

static void credits_update(float elapsed, float dt)
{
    (void)elapsed;
    if (g_phase == PHASE_VIDEO) {
        if (!m64_video_update(&g_video, dt)) g_phase = PHASE_HOLD;
    } else {
        g_hold_t += dt;
    }
}

const PMDemoShot pm_credits_shot = {
    .name     = "credits",
    .duration = SHOT_DURATION,
    .setup    = credits_setup,
    .teardown = credits_teardown,
    .update   = credits_update,
    // .draw is NULL — see pm_narration_shot for why (nothing 3D here).
};

void pm_credits_draw2d(int w, int h)
{
    if (g_phase == PHASE_VIDEO) {
        m64_video_draw(&g_video, w, h);
        return;
    }

    // The static card, once the clip ends (or never existed). Same panel
    // chrome draw_title uses in pm_screens.c, so this reads as the same
    // game's UI rather than a different screen wearing different clothes.
    m64_gui_panel(w / 2 - 100, h / 2 - 30, 200, 60, PM_UI_PANEL, PM_UI_BORDER);
    m64_gui_text(w / 2 - 60, h / 2 - 10, PM_UI_INK, "PETABYTE MADNESS");
    m64_gui_text(w / 2 - 44, h / 2 + 12, PM_UI_INK, "a DeMoD LLC game");
}
