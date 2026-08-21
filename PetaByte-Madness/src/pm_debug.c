// SPDX-License-Identifier: MPL-2.0
//
// pm_debug.c — see pm_debug.h.

#include "pm_debug.h"

#ifdef KILN_DEBUG

#include <libdragon.h>

#include <kiln/kiln_actor.h>
#include <kiln/kiln_clip.h>
#include <kiln/kiln_debugdraw.h>
#include <kiln/kiln_gui.h>

#include "pm_camkey.h"
#include "pm_cine.h"
#include "pm_demo.h"
#include "pm_lab.h"
#include "pm_models.h"
#include "pm_music.h"

// Both name tables used to live here as hand-maintained arrays parallel to
// enums they were not derived from, and both drifted — see PM_SCREEN_LIST
// (pm_screens.h) and PM_MODEL_LIST (pm_models.h) for what each drift cost.
// They are now generated from those lists and reached through
// pm_screen_name() / pm_models_name(), so there is nothing left in this
// file to keep in step with anything.

static int   g_on = 1;   // on by default: a debug ROM is built to be read
static float g_fps;

// ── The spatial layers ─────────────────────────────────────────────────
// A bitmask rather than one enum, because the combinations are the useful
// part: brushes + anchors answers "is the blocker on the bench", camera +
// anchors answers "does this shot ever frame the machine", and all of them at
// once is unreadable, which is why it is not the default.
#define PM_DD_BRUSHES  0x01
#define PM_DD_ACTORS   0x02
#define PM_DD_CAMERA   0x04
#define PM_DD_ANCHORS  0x08

// Cycled rather than toggled per-layer: four independent toggles need four
// chords this pad does not have spare, and the useful sets are few. The
// sequence deliberately starts at "off" so a debug ROM still boots to a clean
// picture — the text panel is on by default because it cannot obscure
// anything, and lines can.
static const uint8_t DD_SETS[] = {
    0,
    PM_DD_CAMERA  | PM_DD_ANCHORS,                 // the cutscene set
    PM_DD_BRUSHES | PM_DD_ANCHORS,                 // the collision set
    PM_DD_ACTORS  | PM_DD_ANCHORS,                 // the population set
    PM_DD_BRUSHES | PM_DD_ACTORS | PM_DD_CAMERA | PM_DD_ANCHORS,
};
static const char *const DD_SET_NAMES[] = {
    "off", "cam", "clip", "actors", "all",
};

// Which set is on at boot. Normally 0 (off), because a debug ROM should still
// show the game. `PM_DD=<index>` overrides it — indices are the DD_SETS order
// above: 0 off, 1 cam, 2 clip, 3 actors, 4 all.
//
// It is a build flag rather than only a chord because the chord needs a working
// controller, and the situation where these layers are most wanted is a capture
// run — `./dev shot` has no input path at all, and `./dev drive`'s uinput -> SDL
// -> ares binding chain is fragile enough that tools/n64-drive.sh's header is
// mostly about it. A layer you can only reach by pressing two buttons is a
// layer no automated capture can ever photograph.
#ifndef PM_DD_INITIAL
#define PM_DD_INITIAL 0
#endif
static int g_dd_set = PM_DD_INITIAL;
static uint16_t g_dd_drawn, g_dd_clipped;

// C-Left + C-Right together. The game binds the C cluster individually
// (kiln_camera and the file menu use them) but never two at once, so this
// cannot fire during ordinary play — the same reasoning kiln_console.h
// gives for its four-button chord, with fewer buttons because this
// toggles rather than opens a text field.
#define TOGGLE_CHORD (KILN_BTN_CL | KILN_BTN_CR)

// C-Up + C-Down, the other pair on the same cluster and equally unused as a
// pair. Advances DD_SETS.
#define LAYER_CHORD  (KILN_BTN_CU | KILN_BTN_CD)

void pm_debug_input(const KilnInput *in)
{
    if (!in) return;
    if ((in->edges & TOGGLE_CHORD) && (in->buttons & TOGGLE_CHORD) == TOGGLE_CHORD)
        g_on = !g_on;
    if ((in->edges & LAYER_CHORD) && (in->buttons & LAYER_CHORD) == LAYER_CHORD)
        g_dd_set = (g_dd_set + 1) % (int)(sizeof DD_SETS / sizeof DD_SETS[0]);
}

void pm_debug_dd_counts(uint16_t *drawn, uint16_t *clipped)
{
    if (drawn)   *drawn   = g_dd_drawn;
    if (clipped) *clipped = g_dd_clipped;
}

// ── The named world positions the intro is keyed to ────────────────────
// Every one of these is a constant some other file derives a camera or a
// placement from, and every one of them is currently unverifiable except by
// building a ROM and looking. Drawing them as axis gizmos with labels is the
// whole point of the ANCHORS layer: "is the scanner where pm_intake.c thinks
// it is" becomes a glance instead of an experiment.
//
// Every position here comes from pm_lab.h, which derives them from
// pm_lab_gen.h — the header dank_lab_gen.py emits by measuring the mesh it
// built. So the gizmos mark where the GENERATOR put things, which is the only
// useful thing to draw: a marker drawn from a number typed into this file would
// confirm the number rather than the geometry.
static void draw_anchors(void)
{
    const struct { fm_vec3_t p; const char *name; } A[] = {
        { {{ 0.0f, 0.0f, 0.0f }},                                  "origin" },
        { {{ PM_LAB_DESK_X, PM_LAB_DESK_Y, PM_LAB_DESK_Z }},       "desk"   },
        { {{ PM_LAB_ARMS_X, PM_LAB_ARMS_Y, PM_LAB_ARMS_Z }},       "arms"   },
        // The bore mouth: where the walk-up path ends and what the handoff
        // shot should be looking at.
        { {{ PM_MRI_X, PM_MRI_Y, PM_MRI_Z_FACE }},                 "mri"    },
    };
    for (int i = 0; i < (int)(sizeof A / sizeof A[0]); i++) {
        kiln_dd_axes(A[i].p, 32.0f);
        kiln_dd_text(A[i].p, RGBA32(220, 220, 120, 255), "%s", A[i].name);
    }

    // The room's own extents, so "the camera is outside the room" stops being
    // something you deduce from six numbers in the text panel.
    kiln_dd_aabb((fm_vec3_t){{ PM_LAB_REAL_X0, PM_LAB_REAL_Y0, PM_LAB_REAL_Z0 }},
                (fm_vec3_t){{ PM_LAB_REAL_X1, PM_LAB_REAL_Y1, PM_LAB_REAL_Z1 }},
                RGBA32(90, 110, 140, 140));
}

static void draw_brushes(void)
{
    uint16_t n = 0;
    const KilnBrush *b = pm_lab_brushes(&n);
    if (!b) return;
    for (uint16_t i = 0; i < n; i++)
        kiln_dd_aabb(b[i].mins, b[i].maxs, RGBA32(255, 96, 96, 150));
}

static void draw_actors(void)
{
    // Category order is the pool's own iteration order, and the colour is the
    // category — so "the guards spawned as PROPs" is visible without reading
    // a profile table.
    static const struct { uint8_t cat; color_t col; const char *tag; } CATS[] = {
        { KILN_ACTOR_CAT_PLAYER, { .r = 120, .g = 220, .b = 255, .a = 200 }, "P" },
        { KILN_ACTOR_CAT_ENEMY,  { .r = 255, .g = 110, .b = 110, .a = 200 }, "E" },
        { KILN_ACTOR_CAT_NPC,    { .r = 255, .g = 210, .b = 120, .a = 200 }, "N" },
        { KILN_ACTOR_CAT_PROP,   { .r = 160, .g = 255, .b = 160, .a = 200 }, "R" },
        { KILN_ACTOR_CAT_BOSS,   { .r = 255, .g = 120, .b = 255, .a = 200 }, "B" },
    };

    // No kiln_actor API reports an actor's collision extents — actors carry a
    // transform, not a volume — so this draws a fixed man-sized box. It marks
    // WHERE an actor is, not how big it is, and saying so here is better than
    // letting the box be read as bounds it is not.
    const fm_vec3_t HALF = {{ 16.0f, 56.0f, 16.0f }};

    for (int c = 0; c < (int)(sizeof CATS / sizeof CATS[0]); c++) {
        for (KilnActor *a = kiln_actor_first(CATS[c].cat); a;
             a = kiln_actor_next(a)) {
            const fm_vec3_t p = a->xform.pos;
            fm_vec3_t centre = p;
            centre.v[1] += HALF.v[1];
            kiln_dd_box(centre, HALF, CATS[c].col);

            // A facing tick, from the transform's own yaw about +Y. An actor
            // pointing the wrong way is one of the two things that make a
            // spawn look broken (the other is being in a wall, above).
            const float yaw = a->xform.rot_angle;
            fm_vec3_t tip = centre;
            tip.v[0] += fm_sinf(yaw) * 40.0f;
            tip.v[2] += fm_cosf(yaw) * 40.0f;
            kiln_dd_line(centre, tip, CATS[c].col);
            kiln_dd_text(centre, CATS[c].col, "%s%u", CATS[c].tag, a->profile_id);
        }
    }
}

/** How many points to sample per key when drawing the flown curve. Eight puts
 *  the sample spacing well under any bulge worth seeing while keeping the whole
 *  path under kiln_debugdraw's measured budget (about 37 lines plus a dozen
 *  labels costs a 60 fps scene six frames). */
#define PATH_SAMPLES_PER_KEY 8
#define PATH_SAMPLES_MAX     96

static void draw_camera_path(void)
{
    const PMDemoShot *shot = pm_demo_current();
    if (!shot || !shot->keys || shot->key_count < 1) return;

    // ── Two curves, and the gap between them is the point ──────────────
    // DIM: the straight-line hull through the keys. BRIGHT: the path the camera
    // actually flies, sampled from pm_camkey_sample — the SAME function
    // pm_demo_apply feeds the camera, so this is the real curve rather than one
    // that resembles it.
    //
    // This comment used to say the flown curve was not drawn and that the
    // difference "is precisely the overshoot". It was right, and drawing only
    // one of the two meant the difference could be reasoned about and not seen.
    // pm_intake.c carries three hand-inserted midpoint keys placed by eye to
    // suppress that bulge; this is what would have placed them.
    for (uint8_t i = 0; i + 1 < shot->key_count; i++)
        kiln_dd_line(shot->keys[i].eye, shot->keys[i + 1].eye,
                    RGBA32(70, 110, 140, 150));

    const int loop = pm_demo_looping();
    int n = shot->key_count * PATH_SAMPLES_PER_KEY;
    if (n > PATH_SAMPLES_MAX) n = PATH_SAMPLES_MAX;
    if (n > 1 && shot->duration > 0.0f) {
        fm_vec3_t prev_eye, prev_look;
        for (int i = 0; i < n; i++) {
            const float t = shot->duration * (float)i / (float)(n - 1);
            fm_vec3_t eye, look;
            pm_camkey_sample(shot->keys, shot->key_count, loop, t, &eye, &look);
            if (i > 0) {
                kiln_dd_line(prev_eye, eye, RGBA32(120, 220, 255, 220));
                // The LOOK path too. A camera that feels drunk is usually its
                // aim swinging, not its position — and the look targets go
                // through the same spline, so they overshoot the same way and
                // are just as invisible as a column of numbers.
                kiln_dd_line(prev_look, look, RGBA32(255, 140, 90, 130));
            }
            prev_eye = eye;
            prev_look = look;
        }
    }

    for (uint8_t i = 0; i < shot->key_count; i++) {
        kiln_dd_point(shot->keys[i].eye, 3, RGBA32(255, 255, 255, 255));
        // Sightline from each eye key to what it looks at. Where these cross
        // a wall, the shot opens inside geometry — the defect pm_debug.h's
        // header names as "a lab shot opening 69 units behind a wall".
        kiln_dd_line(shot->keys[i].eye, shot->keys[i].look,
                    RGBA32(255, 200, 80, 90));
        kiln_dd_point(shot->keys[i].look, 2, RGBA32(255, 200, 80, 200));
        // "k2 5.4" — key index and its time. NOT "2@5.4": libdragon's
        // FONT_BUILTIN_DEBUG_MONO has no '@' glyph and silently renders it as
        // '0', so "2@5.4" came out as "205.4" and read as a coordinate.
        kiln_dd_text(shot->keys[i].eye, RGBA32(200, 240, 255, 255),
                    "k%u %.1f", i, shot->keys[i].t);
    }
}

void pm_debug_draw3d(const PMApp *app, const KilnScene *scene,
                     int screen_w, int screen_h)
{
    (void)app;
    if (!g_on || !scene) return;
    const uint8_t layers = DD_SETS[g_dd_set];
    if (!layers) { g_dd_drawn = g_dd_clipped = 0; return; }

    kiln_dd_begin(scene, screen_w, screen_h);
    // Anchors first, so the denser layers draw over them rather than under.
    if (layers & PM_DD_ANCHORS) draw_anchors();
    if (layers & PM_DD_BRUSHES) draw_brushes();
    if (layers & PM_DD_ACTORS)  draw_actors();
    if (layers & PM_DD_CAMERA)  draw_camera_path();
    g_dd_drawn   = kiln_dd_drawn();
    g_dd_clipped = kiln_dd_clipped();
    kiln_dd_end();
}

void pm_debug_draw(const PMApp *app, const KilnScene *scene,
                   const KilnFpsCam *fps, const PMVeil *veil,
                   float dt, int screen_w, int screen_h)
{
    (void)screen_h;
    // Smoothed, because a per-frame reciprocal flickers too fast to read
    // and the number is only useful as a trend.
    if (dt > 0.0f) g_fps += ((1.0f / dt) - g_fps) * 0.1f;
    // Two separate buffers, because both can appear in one printf argument
    // list and a single shared one would render the same string twice.
    static char f1buf[16], f0buf[16];
    if (!g_on) return;

    const color_t bg  = RGBA32(0, 0, 0, 168);
    const color_t key = RGBA32(120, 200, 255, 255);
    const color_t val = RGBA32(235, 235, 235, 255);
    const color_t bad = RGBA32(255, 96, 96, 255);

    const int x = 4, w = screen_w - 8;
    kiln_gui_panel(x, 4, w, 54, bg, RGBA32(60, 70, 90, 200));

    // The uptime and the frame rate are the only two things on this overlay
    // that cannot come out the same twice, so a build that exists to produce a
    // diffable capture (PM_SHOT_AT) prints them as dashes instead. Measured
    // before it was written: two captures of one PM_SHOT_AT ROM differed in
    // 319 pixels and every one of them was in these two fields.
    //
    // Blanked rather than dropped, so the line keeps its shape and a reader
    // comparing a repro capture against an interactive one is looking at the
    // same columns.
    const int repro = pm_cine_repro();
    kiln_gui_text(x + 4, 14, key, "%s %s  fade %.2f  %s fps  dd:%s %u/%u",
                 pm_screen_name(app->screen),
                 repro ? "--s" : (snprintf(f1buf, sizeof f1buf, "%.1fs",
                                           app->screen_t), f1buf),
                 app->fade,
                 repro ? "--" : (snprintf(f0buf, sizeof f0buf, "%.0f",
                                          (double)g_fps), f0buf),
                 // Which spatial layers are on, and how many primitives they
                 // drew vs dropped as off-screen. Both halves matter: an
                 // "actors 0/0" reads as an empty pool and an "actors 0/36" as
                 // a camera pointed elsewhere, and on a dark frame those are
                 // otherwise the same picture.
                 DD_SET_NAMES[g_dd_set], g_dd_drawn, g_dd_clipped);

    // The camera the scene is actually being built from — cam_pos/cam_target
    // are what kiln_scene_update reads, whichever module wrote them, so this
    // reports the effective camera rather than whichever of the two the
    // screen is nominally driving.
    kiln_gui_text(x + 4, 24, val, "eye %6.0f %6.0f %6.0f  ->%6.0f %6.0f %6.0f",
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
    kiln_gui_text(x + 4, 34,
                 (wide && scene->far_z < 2000.0f) ? bad : val,
                 "near %.0f far %.0f fov %.0f  veil %.2f%s",
                 scene->near_z, scene->far_z, scene->fov_deg,
                 veil ? veil->t : 0.0f,
                 (veil && veil->forced) ? " FORCED" : "");

    if (fps) {
        // The installed brush count, RED at zero. This is the line that would
        // have found in one glance what took the whole life of PM_SCREEN_PLAY
        // to notice: the corridor's .map asset shipped under a different
        // filename than the ROM opened, so the clip world was empty, every
        // trace reported "nothing in the way", and the player fell forever
        // behind a perfectly working HUD. An empty clip world is legitimate
        // (a scene with no geometry) which is exactly why nothing else can
        // report it.
        const uint16_t nb = kiln_clip_world_count();
        kiln_gui_text(x + 4, 44, nb == 0 ? bad : val,
                     "fps-cam %6.0f %6.0f %6.0f yaw %.2f %s  clip %u",
                     fps->pos.v[0], fps->pos.v[1], fps->pos.v[2], fps->yaw,
                     fps->on_ground ? "grounded" : "air", nb);
    } else {
        // Which form of the theme is sounding (pm_music.h). Worth a line
        // because the two are indistinguishable from a screenshot and the
        // failure mode — an asset that did not load — is silence, which
        // looks exactly like "the music has not started yet". Red is the
        // case that matters: neither file is in this ROM.
        const int ms = pm_music_state();
        // Plus how many baked veil palettes loaded. Red at zero, which is the
        // case that matters: the filter's palette-swap half — the thing
        // VEIL_DESIGN.md calls "the one idea" — silently degrades to fog and a
        // far-plane rebate when the .pal assets are absent, and those two look
        // like the whole effect if you have never seen the other half.
        const int np = pm_veil_palettes_loaded();
        kiln_gui_text(x + 4, 44, (ms < 0 || np == 0) ? bad : val,
                     "music %s   veil-pal %d/%d",
                     ms < 0 ? "NONE LOADED"
                            : ms == 1 ? "score (xm64)"
                            : ms == 2 ? "recording (wav64)"
                                      : "silent",
                     np, PM_VEIL_PAL_COUNT);
    }

    // Model residency. A dash is "never asked for", a name is loaded, and
    // a name in red is the case that matters: something asked for it and
    // got NULL, so the screen is drawing an empty scene on purpose and no
    // amount of moving the camera will help.
    int col = x + 4;
    const int row = 66;
    kiln_gui_panel(x, row - 10, w, 16, bg, RGBA32(60, 70, 90, 200));
    for (int i = 0; i < PM_MODEL_COUNT; i++) {
        const int st = pm_models_status((PMModelId)i);
        kiln_gui_text(col, row, st < 0 ? bad : (st > 0 ? val : key),
                     st == 0 ? "-" : pm_models_name((PMModelId)i));
        col += (st == 0) ? 12 : 38;
    }
}

#endif // KILN_DEBUG
