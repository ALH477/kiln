// SPDX-License-Identifier: MPL-2.0
//
// pm_lab.c — see pm_lab.h.

#include "pm_lab.h"

#include <libdragon.h>
#include <string.h>
#include <t3d/t3d.h>
#include <t3d/t3dmodel.h>

#include <m64/m64_audio.h>
#include <m64/m64_clip.h>
#include <m64/m64_context.h>
#include <m64/m64_dialogue.h>
#include <m64/m64_engine.h>
#include <m64/m64_gui.h>
#include <m64/m64_skel.h>
#include <m64/m64_surface.h>

#include "pm_hud.h"
#include "pm_models.h"
#include "pm_screens.h"  // PM_CH_STORY
#include "pm_types.h"

// The surgery OST. Starts here — the moment the player steps into the lab
// — and runs straight through INTAKE's climb-in without restarting; it is
// stopped from the outside, in pm_screens.c's enter_credits_from_intake,
// because its owning lifetime is the whole visit, not just this screen.
#define SURGERY_OST_PATH "rom:/music/surgery_ost.wav64"

// ── Where the lab is ───────────────────────────────────────────────────
// The real extents now live in pm_lab.h (PM_LAB_REAL_*) so pm_demo.c and
// pm_intake.c's cameras can derive from the same numbers this file's own
// collision does. Aliased to the short names below purely so the rest of
// this file doesn't need touching.
//
// Where the FURNITURE is inside this box is still guesswork until someone
// looks at a frame. Every position marked TUNE below is a first pass.
#define LAB_X0   PM_LAB_REAL_X0
#define LAB_X1   PM_LAB_REAL_X1
#define LAB_Y0   PM_LAB_REAL_Y0
#define LAB_Y1   PM_LAB_REAL_Y1
#define LAB_Z0   PM_LAB_REAL_Z0
#define LAB_Z1   PM_LAB_REAL_Z1
#define WALL       (24.0f)

// A 1.8 m man at 64 units per metre: 115 units tall, eye a little below
// the crown. The engine's default fpscam box is ±8 in Y, which is a
// different world scale entirely.
#define EYE_H     (104.0f)
#define HALF_W     (16.0f)

static const M64Brush LAB_BRUSHES[] = {
    // floor / ceiling
    {{{ LAB_X0, LAB_Y0 - WALL, LAB_Z0 }}, {{ LAB_X1, LAB_Y0, LAB_Z1 }},
     PM_SURF_DECK, 0, {0}},
    {{{ LAB_X0, LAB_Y1, LAB_Z0 }}, {{ LAB_X1, LAB_Y1 + WALL, LAB_Z1 }},
     PM_SURF_DECK, 0, {0}},
    // walls
    {{{ LAB_X0 - WALL, LAB_Y0 - WALL, LAB_Z0 }}, {{ LAB_X0, LAB_Y1, LAB_Z1 }},
     PM_SURF_DECK, 0, {0}},
    {{{ LAB_X1, LAB_Y0 - WALL, LAB_Z0 }}, {{ LAB_X1 + WALL, LAB_Y1, LAB_Z1 }},
     PM_SURF_DECK, 0, {0}},
    {{{ LAB_X0, LAB_Y0 - WALL, LAB_Z0 - WALL }}, {{ LAB_X1, LAB_Y1, LAB_Z0 }},
     PM_SURF_DECK, 0, {0}},
    {{{ LAB_X0, LAB_Y0 - WALL, LAB_Z1 }}, {{ LAB_X1, LAB_Y1, LAB_Z1 + WALL }},
     PM_SURF_DECK, 0, {0}},
    // The moon pool. Standing water at the dock end — the LOACH's berth,
    // and where the machine takes him afterwards. Blocked rather than
    // swimmable: there is nothing in this section that wants the player in
    // the water, and an open hole is a way to fall out of the level.
    {{{ LAB_X0 + 40, LAB_Y0, LAB_Z0 + 40 }},
     {{ LAB_X0 + 200, LAB_Y0 + 12, LAB_Z1 - 40 }}, PM_SURF_WATER, 0, {0}},
    // Benches down the long wall. TUNE.
    {{{ -180, LAB_Y0, LAB_Z1 - 80 }}, {{ 60, 56, LAB_Z1 - 20 }},
     PM_SURF_DECK, 0, {0}},
};

// ── Props ──────────────────────────────────────────────────────────────
// Profile ids continue after the demons'; see pm_types.h.
typedef struct {
    uint8_t note_id;  // which text this prop shows
} PMPropState;

static M64Dialogue g_dialogue;
static int         g_mri_activated;

// What the lab has to say. Short, because it is read standing up on a
// 320x240 screen, and because the character is established by the room
// more than by the prose.
static const char *const NOTE_TEXT[][3] = {
    { "Sample log, week 31.",
      "Nothing new in the trench.",
      "Nothing new anywhere." },
    { "The scanner is calibrated.",
      "It was never meant for a",
      "whole person." },
    { "If you are reading this,",
      "I did not come back up",
      "the way I went down." },
};
#define NOTE_COUNT ((int)(sizeof NOTE_TEXT / sizeof NOTE_TEXT[0]))

static void prop_init(M64Actor *self, const M64Dict *args)
{
    (void)args;
    PMPropState *s = (PMPropState *)self->state;
    s->note_id = 0;
    self->health = -1;  // props have no health concept
}

// Props are part of the lab model, not separate meshes: the note on the
// bench is painted into dank_lab.obj. The actor exists so m64_context can
// find it — it has a position and a category and nothing to draw.
static void prop_draw(M64Actor *self) { (void)self; }

static const M64ActorProfile LAB_PROFILES[] = {
    [PM_PROFILE_NOTE - PM_PROFILE_LAB_FIRST] = {
        .name = "note", .category = M64_ACTOR_CAT_PROP,
        .state_size = sizeof(PMPropState),
        .init = prop_init, .draw = prop_draw,
    },
    [PM_PROFILE_MRI - PM_PROFILE_LAB_FIRST] = {
        .name = "mri", .category = M64_ACTOR_CAT_PROP,
        .state_size = sizeof(PMPropState),
        .init = prop_init, .draw = prop_draw,
    },
};

const M64ActorProfile *pm_lab_profiles(void) { return LAB_PROFILES; }
int pm_lab_profile_count(void)
{
    return (int)(sizeof LAB_PROFILES / sizeof LAB_PROFILES[0]);
}

// ── Placement ──────────────────────────────────────────────────────────
// TUNE: every position here is derived from the model's bounding box, not
// from looking at the room. They put things in plausible places; they do
// not yet put things ON the bench they are meant to be on.
static const struct { float x, y, z; uint8_t note; } NOTES[] = {
    {  -20.0f, 60.0f,  110.0f, 0 },
    { -240.0f, 60.0f, -100.0f, 1 },
    {   90.0f, 60.0f,  -90.0f, 2 },
};
static const fm_vec3_t MRI_POS = {{ -330.0f, 40.0f, 60.0f }};

static M64ActorHandle g_notes[NOTE_COUNT];
static M64ActorHandle g_mri = M64_ACTOR_HANDLE_NONE;

fm_vec3_t pm_lab_start_eye(void)
{
    return (fm_vec3_t){{ 60.0f, EYE_H, 120.0f }};
}

// m64_fpscam's yaw is 0 at +Z and forward is (sin yaw, 0, cos yaw), so
// -pi/2 faces -X. That is down the lab's long axis (X runs -452..179)
// toward the moon pool and the MRI, which is where the player should be
// looking the instant the cinematic hands him control — the room's whole
// depth in front of him, and the thing he is going to climb onto at the
// end of it.
float pm_lab_start_yaw(void) { return -1.5708f; }

// The player's body, for every first-person screen in the game.
//
// This is exported (pm_lab.h) rather than kept private because main.c used
// to carry its own second copy of these numbers for PM_SCREEN_PLAY — and
// that copy was authored at a different world scale, giving the player a
// 0.81 m tall, 0.37 m wide box in a world built at 64 units to the metre.
// A knee-high player fits through gaps the level forbids and stands too low
// to frame the room, which is most of what "the camera does not respect
// collisions" looked like. Two copies of one intent is what allowed them to
// disagree, so now there is one.
void pm_lab_body(M64FpsCam *cam)
{
    m64_fpscam_init(cam);
    cam->mins = (fm_vec3_t){{ -HALF_W, -EYE_H, -HALF_W }};
    cam->maxs = (fm_vec3_t){{  HALF_W,  12.0f,  HALF_W }};
    // Scaled from the engine defaults by the same 64-units-per-metre this
    // world uses; the stock 80 u/s is a crawl at this scale.
    cam->move_speed = 190.0f;
    cam->run_speed  = 330.0f;
    cam->gravity    = 1400.0f;
    cam->jump_speed = 420.0f;
}

float pm_lab_eye_height(void) { return EYE_H; }

// The MRI bay's idle-animated arms — same PM_LAB_ARMS_* position pm_demo.c's
// LAB_CINE and pm_intake.c place them at (pm_lab.h), so the machine reads as
// the same fixture across every screen that shows it, not three separate
// props that happen to look alike.
static M64Skel g_arms_skel;
static int     g_arms_ready;

void pm_lab_enter(M64FpsCam *cam, fm_vec3_t eye, float yaw)
{
    m64_clip_set_world(LAB_BRUSHES,
                       (uint16_t)(sizeof LAB_BRUSHES / sizeof LAB_BRUSHES[0]));

    pm_lab_body(cam);
    m64_fpscam_snap(cam, eye, yaw, 0.0f);

    for (int i = 0; i < NOTE_COUNT; i++) {
        g_notes[i] = m64_actor_spawn(
            PM_PROFILE_NOTE,
            (fm_vec3_t){{ NOTES[i].x, NOTES[i].y, NOTES[i].z }}, 0.0f, NULL);
        M64Actor *a = m64_actor_resolve(g_notes[i]);
        if (a) ((PMPropState *)a->state)->note_id = NOTES[i].note;
    }
    g_mri = m64_actor_spawn(PM_PROFILE_MRI, MRI_POS, 0.0f, NULL);

    pm_models_preload(PM_MODEL_LAB_ARMS);
    T3DModel *arms = pm_models_get(PM_MODEL_LAB_ARMS);
    if (arms && !g_arms_ready) {
        m64_skel_create(&g_arms_skel, arms);
        m64_skel_play(&g_arms_skel, "idle", true);
        g_arms_ready = 1;
    }

    const int ost = m64_dfs_exists(SURGERY_OST_PATH)
                        ? m64_sfx_load(SURGERY_OST_PATH) : -1;
    if (ost >= 0) {
        // Priority 255 on the shared story channel — see PM_CH_STORY's
        // comment in pm_screens.h for why sharing it with the narration
        // crawl is safe.
        m64_sfx_play(ost, PM_CH_STORY, 255);
    } else {
        debugf("pm_lab: no %s, running silent\n", SURGERY_OST_PATH);
    }

    memset(&g_dialogue, 0, sizeof g_dialogue);
    g_mri_activated = 0;
}

void pm_lab_leave(void)
{
    for (int i = 0; i < NOTE_COUNT; i++) {
        m64_actor_despawn(g_notes[i]);
        g_notes[i] = M64_ACTOR_HANDLE_NONE;
    }
    m64_actor_despawn(g_mri);
    g_mri = M64_ACTOR_HANDLE_NONE;
    m64_clip_set_world(NULL, 0);

    if (g_arms_ready) {
        m64_skel_destroy(&g_arms_skel);
        g_arms_ready = 0;
    }
}

// ── Frame ──────────────────────────────────────────────────────────────
static M64ContextAction g_action;
static M64ActorHandle   g_focus;

int pm_lab_update(M64FpsCam *cam, const M64Input *in, float dt)
{
    // A dialogue box owns the input while it is up: no walking away
    // mid-sentence, and A advances the text rather than re-triggering the
    // prop the player is still standing in front of.
    if (m64_dialogue_active(&g_dialogue)) {
        m64_dialogue_update(&g_dialogue, dt, in);
        return 0;
    }

    m64_fpscam_update(cam, in, dt);
    if (g_arms_ready) m64_skel_update(&g_arms_skel, dt);

    // ~2 m and a 60 degree cone, in this world's units.
    g_action = m64_context_scan(cam->pos, cam->yaw, 150.0f, 0.52f, &g_focus);

    if (g_action != M64_CTX_NONE && (in->edges & M64_BTN_A)) {
        M64Actor *a = m64_actor_resolve(g_focus);
        if (a && a->profile_id == PM_PROFILE_MRI) {
            g_mri_activated = 1;
            return 1;  // the section ends; pm_intake takes over
        }
        if (a && a->profile_id == PM_PROFILE_NOTE) {
            const PMPropState *s = (const PMPropState *)a->state;
            const int id = s->note_id < NOTE_COUNT ? s->note_id : 0;
            m64_dialogue_start(&g_dialogue, (const char **)NOTE_TEXT[id], 3);
        }
    }
    return 0;
}

void pm_lab_draw3d(void)
{
    T3DModel *model = pm_models_get(PM_MODEL_LAB);
    if (!model) return;

    static M64Transform xform;
    static int ready;
    if (!ready) {
        m64_transform_init(&xform);
        xform.scale = (fm_vec3_t){{ 1.0f, 1.0f, 1.0f }};
        ready = 1;
    }
    m64_transform_push(&xform);
    t3d_model_draw(model);
    m64_transform_pop();

    if (g_arms_ready) {
        static M64Transform arms_x;
        static int arms_ready_x;
        if (!arms_ready_x) { m64_transform_init(&arms_x); arms_ready_x = 1; }
        arms_x.pos = (fm_vec3_t){{ PM_LAB_ARMS_X, PM_LAB_ARMS_Y, PM_LAB_ARMS_Z }};
        arms_x.scale = (fm_vec3_t){{ 1.0f, 1.0f, 1.0f }};
        arms_x.rot_axis = (fm_vec3_t){{ 0.0f, 1.0f, 0.0f }};
        arms_x.rot_angle = 0.0f;
        m64_transform_push(&arms_x);
        m64_skel_draw(&g_arms_skel);
        m64_transform_pop();
    }
}

void pm_lab_draw2d(int w, int h)
{
    if (m64_dialogue_active(&g_dialogue)) {
        m64_dialogue_draw(&g_dialogue);
        return;
    }

    // The A-button prompt. OoT's rule: A always does something, and the
    // player is told what before they press it. Backed by the same panel
    // pm_hud uses, sized for the longer of the two labels, so the prompt
    // reads as this game's UI rather than debug text floating over a room.
    if (g_action != M64_CTX_NONE) {
        M64Actor *a = m64_actor_resolve(g_focus);
        const char *label = (a && a->profile_id == PM_PROFILE_MRI)
                                ? "ACTIVATE" : "READ";
        m64_gui_panel(w / 2 - 58, h - 70, 116, 20, PM_UI_PANEL, PM_UI_BORDER);
        m64_gui_text(w / 2 - 48, h - 64, PM_UI_INK, "A: %s", label);
    }

    // Crosshair — two ticks, matching pm_hud's, same ink.
    const int cx = w / 2, cy = h / 2;
    m64_gui_rect(cx - 4, cy, 3, 1, PM_UI_INK);
    m64_gui_rect(cx + 2, cy, 3, 1, PM_UI_INK);
}
