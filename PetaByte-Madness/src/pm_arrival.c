// SPDX-License-Identifier: MPL-2.0
//
// pm_arrival.c — see pm_arrival.h.

#include "pm_arrival.h"

#include <libdragon.h>
#include <string.h>
#include <t3d/t3d.h>
#include <t3d/t3dmodel.h>

#include <m64/m64_engine.h>
#include <m64/m64_skel.h>

#include "pm_fx.h"
#include "pm_sfx.h"
#include "pm_models.h"
#include "pm_demo.h"
#include "pm_env.h"

// ── Shared drawing ─────────────────────────────────────────────────────
// ── Why a RING of transforms, not one ──────────────────────────────────
// M64Transform owns an UNCACHED matrix that the RSP reads ASYNCHRONOUSLY:
// m64_transform_push records a command referencing that buffer, and the RSP
// consumes it later, when the frame's command list actually runs.
//
// This used to be a single shared M64Transform, on the reasoning that the
// uncached allocation is expensive and one is cheaper than many. That is
// true and it is also unusable: every draw in the frame overwrote the same
// matrix before the RSP had read any of them, so every object in the frame
// was drawn with the LAST object's transform. The island came out wearing a
// palm tree's scale, yaw and position — it appeared to spin and fly around
// the sea, changing every frame as the race resolved differently.
//
// It survived review because a SCREENSHOT cannot show it: any single frame
// is internally consistent, and only the motion between frames reveals it.
// The tell in the stills was there and was missed — the island's apparent
// size swung 5x between captures whose camera radii differed by 14%.
//
// The ring only has to be longer than the most draws any one frame makes
// (the flyover's worst case is the island plus ten palms), so an entry is
// never rewritten inside the frame that is still using it, and by the time
// the index wraps that frame has long since presented.
#define XFORM_RING 16
static M64Transform g_xform[XFORM_RING];
static int          g_xform_ready;
static int          g_xform_next;

static M64Transform *xform(void)
{
    if (!g_xform_ready) {
        for (int i = 0; i < XFORM_RING; i++) m64_transform_init(&g_xform[i]);
        g_xform_ready = 1;
    }
    M64Transform *t = &g_xform[g_xform_next];
    g_xform_next = (g_xform_next + 1) & (XFORM_RING - 1);
    return t;
}

static void draw_at(PMModelId id, fm_vec3_t pos, float scale, float yaw)
{
    T3DModel *m = pm_models_get(id);
    if (!m) return;
    M64Transform *t = xform();
    t->pos = pos;
    t->scale = (fm_vec3_t){{ scale, scale, scale }};
    t->rot_axis = (fm_vec3_t){{ 0.0f, 1.0f, 0.0f }};
    t->rot_angle = yaw;
    m64_transform_push(t);
    t3d_model_draw(m);
    m64_transform_pop();
}

// ── Shot: the submarine ────────────────────────────────────────────────
// Twelve seconds, and the whole shot is one rising move. The camera starts
// BELOW the boat looking up at its belly, which is the angle that makes a
// small sub look like a big one, and ends above the waterline with the
// island filling the frame.
#define SUB_X 0.0f
#define SUB_Z 0.0f

static const PMCamKey SUB_KEYS[] = {
    { 0.0f,  {{ -260.0f, -640.0f,  420.0f }}, {{ SUB_X, -160.0f, SUB_Z }} },
    { 4.0f,  {{ -180.0f, -300.0f,  360.0f }}, {{ SUB_X,  -40.0f, SUB_Z }} },
    { 8.0f,  {{  -90.0f,   40.0f,  300.0f }}, {{ SUB_X,   90.0f, SUB_Z - 200.0f }} },
    { 12.0f, {{  -40.0f,  180.0f,  240.0f }}, {{ SUB_X,  150.0f, SUB_Z - 900.0f }} },
};

static void sub_setup(void)
{
    pm_models_preload(PM_MODEL_LOACH);
    pm_models_preload(PM_MODEL_ISLAND);
    pm_env_init();
    // Bars for the whole arrival. They come in over 0.4 s, which is why
    // this is set here and not on the frame the shot draws.
    pm_fx_letterbox(1.0f);
}

static void sub_draw(float t)
{
    // Same order as the flyover: sky, world, sea. See pm_env.h.
    pm_env_draw_sky(pm_demo_scene());

    // The boat drifts forward through the shot; the camera rises past it.
    const float z = SUB_Z + 40.0f - t * 26.0f;
    draw_at(PM_MODEL_LOACH, (fm_vec3_t){{ SUB_X, -60.0f + t * 14.0f, z }},
            1.0f, 3.14159f);

    // The island, far off and low, so it reads as a destination rather
    // than as scenery. It only clears the frame edge in the last third.
    //
    // The distance is load-bearing and was wrong by an order of magnitude:
    // island_n64.obj is 26 units across, x7.7 to metres, x64 to world units
    // (see PM_ISLAND_HALF_W) = 12,813 wide and 4,954 tall. At the old
    // z = -3200 its half-width subtended about 61 degrees — it did not read
    // as a destination on the horizon, it filled and overflowed the frame.
    // At -30000 it is roughly a sixth of the frame height, which is a place
    // you are travelling towards.
    draw_at(PM_MODEL_ISLAND, (fm_vec3_t){{ 0.0f, -80.0f, -30000.0f }},
            1.0f, 0.6f);

    pm_env_draw_bolt(pm_demo_scene());
    pm_env_draw_sea();
}

const PMDemoShot pm_arrival_sub = {
    .name = "sub", .duration = 12.0f,
    .keys = SUB_KEYS, .key_count = 4,
    .setup = sub_setup, .draw = sub_draw,
    // The island sits at 30,000 units and is the point of the shot, so this
    // is one of the two places in the game that needs the horizon. The
    // near plane stays tight: the LOACH passes within a few hundred units
    // of the lens on the way up.
    .near_z = 20.0f, .far_z = 40000.0f, .exterior = 1,
};

// ── Shot: the beach ────────────────────────────────────────────────────
// The beat sheet, in seconds. Kept as named constants rather than magic
// numbers in three places, because every one of them is referenced by the
// camera keys, the animation switch AND the fx triggers, and a beat that
// drifts between those three reads as the animation being wrong.
#define B_CRASH     0.6f   // the hull hits the sand
#define B_CLIMB     2.0f   // he comes out — `notice`
#define B_FIRE      4.2f   // guard A
#define B_SLASH     6.0f   // guard B
#define B_SHOUT     7.6f   // held
#define B_FACE      9.2f   // the camera drives in
#define B_BINARY   10.6f   // white flash, then MADNESS
#define B_END      13.0f

static const fm_vec3_t GUARD_A = {{  120.0f, 0.0f, -150.0f }};
static const fm_vec3_t GUARD_B = {{ -140.0f, 0.0f, -210.0f }};
static const fm_vec3_t HERO    = {{    0.0f, 0.0f,  -40.0f }};

static const PMCamKey BEACH_KEYS[] = {
    // wide, low, the sub coming in off the water
    { 0.0f,      {{  420.0f,  90.0f,  340.0f }}, {{    0.0f,  60.0f,  -60.0f }} },
    // the impact, closer
    { B_CRASH,   {{  300.0f,  70.0f,  260.0f }}, {{    0.0f,  50.0f,  -60.0f }} },
    // the guards, who do not understand what they are looking at
    { B_CLIMB,   {{  240.0f,  80.0f,   60.0f }}, {{   20.0f,  70.0f, -140.0f }} },
    // behind him for the shot
    { B_FIRE,    {{ -110.0f, 100.0f,  120.0f }}, {{  110.0f,  70.0f, -150.0f }} },
    // whipping round for the slash
    { B_SLASH,   {{  150.0f,  90.0f,   40.0f }}, {{ -130.0f,  70.0f, -200.0f }} },
    // low and looking up at him for the scream
    { B_SHOUT,   {{   90.0f,  30.0f,   90.0f }}, {{    0.0f, 130.0f,  -50.0f }} },
    // and into the face
    { B_FACE,    {{    6.0f, 150.0f,   26.0f }}, {{    0.0f, 150.0f,  -40.0f }} },
    { B_BINARY,  {{    1.0f, 152.0f,    8.0f }}, {{    0.0f, 152.0f,  -40.0f }} },
    { B_END,     {{    1.0f, 152.0f,    8.0f }}, {{    0.0f, 152.0f,  -40.0f }} },
};

static M64Skel g_hero_skel;
static int     g_hero_ready;
static const char *g_hero_clip;   // what is playing, so we only switch once
static int     g_guard_a_down;
static int     g_guard_b_down;
static int     g_flashed;
static int     g_crashed;

static void beach_setup(void)
{
    pm_env_init();

    pm_models_preload(PM_MODEL_LOACH);
    pm_models_preload(PM_MODEL_ISLAND);
    pm_models_preload(PM_MODEL_GUARD);

    T3DModel *centaur = pm_models_get(PM_MODEL_CENTAUR);
    if (centaur && !g_hero_ready) {
        m64_skel_create(&g_hero_skel, centaur);
        g_hero_ready = 1;
    }
    g_hero_clip = NULL;
    g_guard_a_down = g_guard_b_down = 0;
    g_flashed = 0;
    g_crashed = 0;
    pm_fx_letterbox(1.0f);
}

static void beach_teardown(void) { pm_fx_letterbox(0.0f); }

/** Switch clips only on a change — m64_skel_play tears down and rebuilds
 *  the T3DAnim, so calling it every frame would restart the animation on
 *  every frame and nothing would ever visibly play.
 *
 *  Returns 1 on the frame the clip actually changed, so a caller can hang
 *  a one-shot (a sound, a shake) off the switch without tracking it. */
static int play_once(const char *clip, bool loop)
{
    if (!g_hero_ready || g_hero_clip == clip) return 0;
    g_hero_clip = clip;
    m64_skel_play(&g_hero_skel, clip, loop);
    return 1;
}

static void beach_update(float t, float dt)
{
    if (t >= B_BINARY) {
        play_once("shout", false);
    } else if (t >= B_SHOUT) {
        if (play_once("shout", false)) pm_sfx_play(PM_SFX_SCREAM);
    } else if (t >= B_SLASH) {
        play_once("slash", false);
    } else if (t >= B_FIRE) {
        play_once("fire", false);
    } else if (t >= B_CLIMB) {
        play_once("notice", false);
    } else {
        play_once("idle", true);
    }

    if (g_hero_ready) m64_skel_update(&g_hero_skel, dt);

    // ── The punctuation ────────────────────────────────────────────────
    // Each of these fires once, on the frame the beat is crossed. The
    // guards are removed on the hit frame rather than animated dying:
    // there is no death animation in the mob set, and a body that simply
    // stops being there while the camera is still moving is a cut, which
    // is what a 1998 game would have done anyway.
    // The hull hitting sand. Big, brief, and the only shake in the shot
    // that is not a weapon. Reset in beach_setup rather than kept in a
    // function-static: a second run of this shot (a new profile on the
    // same boot) would otherwise skip the impact entirely and nothing
    // would say why.
    if (t >= B_CRASH && !g_crashed) {
        g_crashed = 1;
        pm_fx_shake(26.0f, 0.6f);
        pm_sfx_play(PM_SFX_HULL_IMPACT);
    }
    if (t >= B_FIRE && !g_guard_a_down) {
        g_guard_a_down = 1;
        pm_fx_shake(14.0f, 0.25f);
        pm_fx_hitstop(0.10f);
        pm_fx_flash(RGBA32(0xFF, 0xE8, 0xC0, 0x90), 0.12f);
        pm_sfx_play(PM_SFX_SHOTGUN);
    }
    if (t >= B_SLASH && !g_guard_b_down) {
        g_guard_b_down = 1;
        pm_fx_shake(10.0f, 0.22f);
        // Longer than the shot's. A blade landing wants more hang time
        // than a gun; the stop IS the weight.
        pm_fx_hitstop(0.16f);
        pm_fx_flash(RGBA32(0xC8, 0x18, 0x1E, 0x70), 0.18f);
        pm_sfx_play(PM_SFX_BLADE);
    }
    if (t >= B_BINARY && !g_flashed) {
        g_flashed = 1;
        pm_fx_flash(RGBA32(0xFF, 0xFF, 0xFF, 0xFF), 0.25f);
        pm_fx_binary(B_END - B_BINARY);
    }
}

static void beach_draw(float t)
{
    pm_env_draw_sky(pm_demo_scene());

    // The beach is the island's SHORELINE, so the island has to be placed
    // by its edge rather than its centre. It is a radial island — the mesh
    // runs 0..4,954 units in Y over a 6,406-unit radius, high in the middle
    // and at sea level at the rim — so putting its centre at z = -900, as
    // this did, stood every actor 900 units inside the footprint, buried in
    // the hill. Backing the centre off to just inside the rim leaves the
    // action on the low outer sand with the island rising behind it, which
    // is the shot these camera keys were cut for.
    draw_at(PM_MODEL_ISLAND,
            (fm_vec3_t){{ 0.0f, -40.0f, -(PM_ISLAND_HALF_W - 400.0f) }},
            1.0f, 0.6f);

    // The sub: skidding in, then stopped and canted over in the sand.
    const float slide = t < B_CRASH ? (B_CRASH - t) * 700.0f : 0.0f;
    draw_at(PM_MODEL_LOACH,
            (fm_vec3_t){{ 40.0f + slide * 0.6f, 10.0f, 60.0f + slide }},
            1.0f, 3.4f);

    if (!g_guard_a_down)
        draw_at(PM_MODEL_GUARD, GUARD_A, 1.0f, 2.6f);
    if (!g_guard_b_down)
        draw_at(PM_MODEL_GUARD, GUARD_B, 1.0f, 3.6f);

    // He is inside the hull until he climbs out.
    if (t >= B_CLIMB && g_hero_ready) {
        M64Transform *x = xform();
        x->pos = HERO;
        x->scale = (fm_vec3_t){{ 1.0f, 1.0f, 1.0f }};
        x->rot_axis = (fm_vec3_t){{ 0.0f, 1.0f, 0.0f }};
        // Turns from guard A to guard B between the two kills, so the
        // slash lands on someone he is actually facing.
        x->rot_angle = (t < B_SLASH) ? 2.5f : 3.9f;
        m64_transform_push(x);
        m64_skel_draw(&g_hero_skel);
        m64_transform_pop();
    }
}

const PMDemoShot pm_arrival_beach = {
    .name = "beach", .duration = B_END,
    .keys = BEACH_KEYS, .key_count = 9,
    .setup = beach_setup, .teardown = beach_teardown,
    .draw = beach_draw, .update = beach_update,
    // The action is all inside 600 units, but the island rises behind it
    // and its far side is ~13,000 out, so the default 4,000 would cut the
    // backdrop off mid-hill. The near plane has to be tight because the
    // last two keys put the lens 46 units from Horner's face.
    .near_z = 8.0f, .far_z = 20000.0f, .exterior = 1,
};

void pm_arrival_close(void)
{
    if (g_hero_ready) {
        m64_skel_destroy(&g_hero_skel);
        g_hero_ready = 0;
        g_hero_clip = NULL;
    }
}
