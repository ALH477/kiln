// SPDX-License-Identifier: MPL-2.0
//
// pm_debug.c — see pm_debug.h.

#include "pm_debug.h"

#ifdef M64_DEBUG

#include <libdragon.h>

#include <m64/m64_gui.h>

#include "pm_models.h"
#include "pm_music.h"

// Same hand-maintained-table risk as MODEL_NAMES below, and it had the same
// bug: PM_SCREEN_NARRATION and PM_SCREEN_CREDITS landed in the PMScreen enum
// (pm_screens.h) with no matching entries here, so every name from
// "LAB_CINE" on was reporting the PREVIOUS screen's name instead of its
// own — silently, since the bounds check below only guards against running
// off the end of a too-short array, not against the array being internally
// misaligned with the enum it's meant to label. Caught by an actual
// emulator run showing "LAB_CINE" while narration text was on screen.
static const char *const SCREEN_NAMES[] = {
    "BOOT", "TITLE", "ATTRACT", "FILE", "NARRATION", "LAB_CINE",
    "LAB", "INTAKE", "CREDITS", "SUB", "BEACH", "PLAY",
};

// Short enough to fit four to a line at 320 px. Order matches PMModelId.
//
// This table is sized off PM_MODEL_COUNT but its contents are a SEPARATE,
// hand-maintained list — nothing enforces the two staying in step, and
// they didn't: PM_MODEL_LAB_ARMS landed in the enum with no matching entry
// here, so every name from "guard" on quietly shifted one slot early and
// the array fell one short of PM_MODEL_COUNT. The missing final slot
// (PM_MODEL_STORM, C-zero-initialised to NULL) was never a problem until
// the very first frame this overlay actually ran, at which point
// m64_gui_text got NULL as a format string and read byte 0 of address
// zero — a boot-time crash on every build, caught only once the ROM was
// finally run in an emulator instead of just `nix build`/`nix flake check`.
static const char *const MODEL_NAMES[PM_MODEL_COUNT] = {
    "isle", "palm", "cent", "loch", "lab", "horn", "arms", "guard", "logo",
    "sky", "sea", "bolt",
};

static int   g_on = 1;   // on by default: a debug ROM is built to be read
static float g_fps;

// C-Left + C-Right together. The game binds the C cluster individually
// (m64_camera and the file menu use them) but never two at once, so this
// cannot fire during ordinary play — the same reasoning m64_console.h
// gives for its four-button chord, with fewer buttons because this
// toggles rather than opens a text field.
#define TOGGLE_CHORD (M64_BTN_CL | M64_BTN_CR)

void pm_debug_input(const M64Input *in)
{
    if (!in) return;
    if ((in->edges & TOGGLE_CHORD) && (in->buttons & TOGGLE_CHORD) == TOGGLE_CHORD)
        g_on = !g_on;
}

void pm_debug_draw(const PMApp *app, const M64Scene *scene,
                   const M64FpsCam *fps, const PMVeil *veil,
                   float dt, int screen_w, int screen_h)
{
    (void)screen_h;
    // Smoothed, because a per-frame reciprocal flickers too fast to read
    // and the number is only useful as a trend.
    if (dt > 0.0f) g_fps += ((1.0f / dt) - g_fps) * 0.1f;
    if (!g_on) return;

    const color_t bg  = RGBA32(0, 0, 0, 168);
    const color_t key = RGBA32(120, 200, 255, 255);
    const color_t val = RGBA32(235, 235, 235, 255);
    const color_t bad = RGBA32(255, 96, 96, 255);

    const int x = 4, w = screen_w - 8;
    m64_gui_panel(x, 4, w, 54, bg, RGBA32(60, 70, 90, 200));

    const char *name = (app->screen >= 0 &&
                        app->screen < (int)(sizeof SCREEN_NAMES / sizeof SCREEN_NAMES[0]))
                     ? SCREEN_NAMES[app->screen] : "?";
    m64_gui_text(x + 4, 14, key, "%s %.1fs  fade %.2f  %.0f fps",
                 name, app->screen_t, app->fade, g_fps);

    // The camera the scene is actually being built from — cam_pos/cam_target
    // are what m64_scene_update reads, whichever module wrote them, so this
    // reports the effective camera rather than whichever of the two the
    // screen is nominally driving.
    m64_gui_text(x + 4, 24, val, "eye %6.0f %6.0f %6.0f  ->%6.0f %6.0f %6.0f",
                 scene->cam_pos.v[0], scene->cam_pos.v[1], scene->cam_pos.v[2],
                 scene->cam_target.v[0], scene->cam_target.v[1],
                 scene->cam_target.v[2]);

    // near/far in red when the far plane is under 2000 units on a screen
    // that frames the island: that IS the veil-clamps-everything defect,
    // and it is invisible in a picture because the island simply is not
    // drawn.
    const int wide = (app->screen == PM_SCREEN_TITLE ||
                      app->screen == PM_SCREEN_ATTRACT ||
                      app->screen == PM_SCREEN_SUB);
    m64_gui_text(x + 4, 34,
                 (wide && scene->far_z < 2000.0f) ? bad : val,
                 "near %.0f far %.0f fov %.0f  veil %.2f%s",
                 scene->near_z, scene->far_z, scene->fov_deg,
                 veil ? veil->t : 0.0f,
                 (veil && veil->forced) ? " FORCED" : "");

    if (fps) {
        m64_gui_text(x + 4, 44, val, "fps-cam %6.0f %6.0f %6.0f yaw %.2f %s",
                     fps->pos.v[0], fps->pos.v[1], fps->pos.v[2], fps->yaw,
                     fps->on_ground ? "grounded" : "air");
    } else {
        // Which form of the theme is sounding (pm_music.h). Worth a line
        // because the two are indistinguishable from a screenshot and the
        // failure mode — an asset that did not load — is silence, which
        // looks exactly like "the music has not started yet". Red is the
        // case that matters: neither file is in this ROM.
        const int ms = pm_music_state();
        m64_gui_text(x + 4, 44, ms < 0 ? bad : val, "music %s",
                     ms < 0 ? "NONE LOADED"
                            : ms == 1 ? "score (xm64)"
                            : ms == 2 ? "recording (wav64)"
                                      : "silent");
    }

    // Model residency. A dash is "never asked for", a name is loaded, and
    // a name in red is the case that matters: something asked for it and
    // got NULL, so the screen is drawing an empty scene on purpose and no
    // amount of moving the camera will help.
    int col = x + 4;
    const int row = 66;
    m64_gui_panel(x, row - 10, w, 16, bg, RGBA32(60, 70, 90, 200));
    for (int i = 0; i < PM_MODEL_COUNT; i++) {
        const int st = pm_models_status((PMModelId)i);
        m64_gui_text(col, row, st < 0 ? bad : (st > 0 ? val : key),
                     st == 0 ? "-" : MODEL_NAMES[i]);
        col += (st == 0) ? 12 : 38;
    }
}

#endif // M64_DEBUG
