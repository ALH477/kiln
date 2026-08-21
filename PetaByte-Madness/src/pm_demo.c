// SPDX-License-Identifier: MPL-2.0
//
// pm_demo.c — see pm_demo.h.

#include "pm_demo.h"

#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmodel.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_skel.h>

#include "pm_models.h"
#include "pm_env.h"
#include "pm_world_gen.h"
#include "pm_lab.h"
#include "pm_types.h"
#include "pm_veil.h"

// ── Director state ─────────────────────────────────────────────────────
static const PMDemoShot *g_shot;
static float g_elapsed;
static int   g_loop;
static int   g_done;

// ── Why a RING of transforms, not one ──────────────────────────────────
// KilnTransform owns an UNCACHED matrix that the RSP reads ASYNCHRONOUSLY:
// kiln_transform_push records a command referencing that buffer, and the RSP
// consumes it later, when the frame's command list actually runs.
//
// This used to be a single shared KilnTransform, on the reasoning that the
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
static KilnTransform g_xform[XFORM_RING];
static int          g_xform_ready;
static int          g_xform_next;

static KilnTransform *xform(void)
{
    if (!g_xform_ready) {
        for (int i = 0; i < XFORM_RING; i++) kiln_transform_init(&g_xform[i]);
        g_xform_ready = 1;
    }
    KilnTransform *t = &g_xform[g_xform_next];
    g_xform_next = (g_xform_next + 1) & (XFORM_RING - 1);
    return t;
}

/** Draw one model at a position/scale/yaw. */
static void draw_at(PMModelId id, fm_vec3_t pos, float scale, float yaw)
{
    T3DModel *model = pm_models_get(id);
    if (!model) return;

    KilnTransform *t = xform();
    t->pos = pos;
    t->scale = (fm_vec3_t){{ scale, scale, scale }};
    t->rot_axis = (fm_vec3_t){{ 0.0f, 1.0f, 0.0f }};
    t->rot_angle = yaw;

    kiln_transform_push(t);
    t3d_model_draw(model);
    kiln_transform_pop();
}

/** Draw one named object out of a model — how the palms get placed
 *  individually. See docs/ASSET_PIPELINE.md on why they are split. */
static void draw_object_at(PMModelId id, const char *object,
                           fm_vec3_t pos, float scale, float yaw)
{
    T3DModel *model = pm_models_get(id);
    if (!model) return;
    T3DObject *obj = t3d_model_get_object(model, object);
    if (!obj) return;

    KilnTransform *t = xform();
    t->pos = pos;
    t->scale = (fm_vec3_t){{ scale, scale, scale }};
    t->rot_axis = (fm_vec3_t){{ 0.0f, 1.0f, 0.0f }};
    t->rot_angle = yaw;

    kiln_transform_push(t);
    t3d_model_draw_object(obj, NULL);
    kiln_transform_pop();
}

// ── Shot 1: the drone flyover ──────────────────────────────────────────
// The title screen's background, and the first thing in the attract reel.
// A slow orbiting descent: high and wide at the start, lower and closer by
// the end, always looking at the island's mass rather than its centre so
// the horizon sits low and the middle of the frame stays clear for the
// skull and the menu.
// The island's footprint is PM_ISLAND_HALF_W wide and PM_ISLAND_TOP tall
// (pm_models.h, both DERIVED from pm_world_gen.h rather than typed — this
// used to be a literal "12,813 units across", which drifted from the real
// generated size the moment the island stopped being the OBJ that number
// described). At KilnScene's 85 degree FOV, holding the whole silhouette
// needs an eye roughly 1.2-1.9x PM_ISLAND_HALF_W out.
//
// The first version of this table put the eye at 1,400 — INSIDE the
// island's footprint and below its peak. It rendered black, which read as
// "the ROM is broken" and sent the hunt somewhere else entirely. Camera
// numbers are only meaningful next to the extents of what they frame.
// The keys are DERIVED, not typed. Every camera bug in this game has been
// a number that described geometry it had no link to: an eye 69 units
// behind a wall, an island placed at a tenth of its size, a key at 1.4
// island radii that flew through the ridges. pm_world_gen.h is emitted by
// the generator that builds the island, so these cannot drift from it.
//
// The orbit: 2.4 radii out puts the island at roughly half the frame at
// this FOV, and an eye above PM_ISLAND_TOP guarantees the camera is over
// the terrain rather than in it — which is the invariant the old table
// broke. flyover_keys() fills the table once at setup.
// ── The orbit ──────────────────────────────────────────────────────────
// Keys are points on a circle, but pm_demo_apply interpolates the eye
// POSITION linearly — so the camera travels the CHORD between two keys,
// not the arc, and every segment dips inside the orbit. At 60 degrees of
// spacing that dip is 1 - cos(30) = 13.4%, which was enough to put a key
// nominally 2.2 island radii out through the ridges at 1.9.
//
// Two corrections, both derived: more keys (so each chord is shorter) and
// a radius scaled by 1/cos(half a segment) so the chord's MIDPOINT — its
// closest approach — lands on the intended radius rather than its ends.
//
// Raised from 12 to 14 when the caye was resized to ~210 m land radius
// (~2x): the orbit radius scales with PM_LAND_RADIUS, so the same segment
// count now spans a proportionally longer chord. More, shorter chords keep
// the per-segment dip (and the ridge-clearance margin it eats into) the
// same fraction of the orbit it always was.
#define FLYOVER_SEGMENTS  14
#define FLYOVER_KEY_COUNT (FLYOVER_SEGMENTS + 1)   // last key == first
static PMCamKey FLYOVER_KEYS[FLYOVER_KEY_COUNT];

// How far out the closest approach should be, in island radii, and how high
// the eye rides above the island's highest point.
// Closer, and lower. 2.45 radii held the whole island comfortably in frame
// with sea all round it, which suited a green headland; a Florida caye is
// flat and low and reads as nothing at all from up there. Coming in to 1.5
// puts the beach and the scrub at a size you can see, and drops the horizon
// so the sky band and the moon path do the work behind it.
// INSIDE the island now, not around it.
//
// Past pm_env.c's FOG_FAR_FRAC * far_z, nothing is visible — an orbit
// outside the coast (1.05 radii) would show an empty grey screen, the caye
// entirely swallowed. The camera instead drifts low over the field itself
// at roughly a quarter of the land radius, and the temple, the gates and
// the palms loom out of the fog as it passes them. That is the shot the
// fog wants. See below for why near_z/far_z have to move in lockstep with
// PM_LAND_RADIUS whenever the island is resized, or this relationship
// breaks silently.
//
// 0.28 keeps it inside the field (PM_FIELD_RADIUS is 0.41 of the land
// radius) and well clear of the temple's 15 m base at the centre.
#define ORBIT_RADII       0.28f
#define ORBIT_WOBBLE      0.08f   // gentle in-and-out, so it is not a lathe
// Altitude as a fraction of the LAND radius, not a multiple of the tower.
// A caye is flat, so a camera placed relative to its highest point ends up
// hundreds of metres over a pancake; placed relative to its width it stays
// in the low-aerial band where the beach and the scrub still read.
// 0.07-0.12 of PM_LAND_RADIUS is a low-aerial altitude at any island size —
// scales with the resize automatically, unlike near_z/far_z below.
// Ground is 2.4 m and the temple is 25.7 m; the camera stays low against both.
// The temple therefore TOWERS over the camera rather than being looked down
// on, which is most of why it reads as ominous rather than as a model.
#define ORBIT_HIGH        0.12f
#define ORBIT_LOW         0.07f

static void flyover_build_keys(void)
{
    const float seg_deg = 360.0f / (float)FLYOVER_SEGMENTS;
    const float half    = seg_deg * 0.5f * 0.017453293f;
    /* Push the keys out so the chord midpoint sits at ORBIT_RADII. */
    const float chord_fix = 1.0f / fm_cosf(half);

    // PM_LAND_RADIUS, not PM_ISLAND_RADIUS: the latter runs out to the
    // submerged shelf, which frames as sea. Orbiting against it quietly put
    // the eye half again too far out — the island came back SMALLER after a
    // change whose whole purpose was to get closer.
    const float base_r = PM_LAND_RADIUS * ORBIT_RADII * chord_fix;
    // Raised from 30s when the caye's land radius roughly doubled: base_r
    // scales with PM_LAND_RADIUS, so the same duration would now cover
    // proportionally more ground per second. 36s keeps the felt pace (world
    // units travelled per second of screen time) close to what it was
    // before the resize, rather than turning one full lap into a rushed
    // fly-by. Feel-tuned, not derived — re-check by eye if the island's
    // scale changes again.
    const float dur    = 36.0f;

    for (int i = 0; i < FLYOVER_KEY_COUNT; i++) {
        const float u = (float)i / (float)FLYOVER_SEGMENTS;   /* 0..1 */
        const float a = (135.0f + 360.0f * u) * 0.017453293f;

        /* One slow in-and-out over the orbit, and one descent-and-rise, so
         * the move has shape without any key being hand-placed. */
        const float r = base_r * (1.0f + ORBIT_WOBBLE * fm_sinf(u * 6.2831853f));
        const float h = PM_LAND_RADIUS
                        * (ORBIT_LOW + (ORBIT_HIGH - ORBIT_LOW)
                           * (0.5f + 0.5f * fm_cosf(u * 6.2831853f)));

        FLYOVER_KEYS[i].t = dur * u;
        FLYOVER_KEYS[i].eye = (fm_vec3_t){ {
            fm_cosf(a) * r, h, fm_sinf(a) * r
        } };
        /* Look at the island's mass, not its centre: aiming a third of the
         * way up keeps the horizon low and the middle of the frame clear
         * for the skull and the menu. */
        // Aim at the land, not at the tower's midpoint: on a flat caye the
        // interesting line is the shore, so the target sits just above the
        // field rather than a third of the way up a 32 m landmark.
        FLYOVER_KEYS[i].look = (fm_vec3_t){ { 0.0f, PM_FIELD_Y * 1.6f, 0.0f } };
    }
}

static void flyover_setup(void)
{
    flyover_build_keys();
    pm_models_preload(PM_MODEL_ISLAND);
    pm_models_preload(PM_MODEL_PALMS);
    pm_env_init();
}

static void flyover_draw(float elapsed)
{
    (void)elapsed;
    // Sky first (it disables depth, so everything paints over it), then the
    // island, then the sea LAST so the island's depth rejects the water
    // behind it rather than the water overdrawing the shore. See pm_env.h.
    pm_env_draw_sky(pm_demo_scene());
    draw_at(PM_MODEL_ISLAND, (fm_vec3_t){{ 0, 0, 0 }}, 1.0f, 0.0f);

    // Palms around the shoreline, placed on the RING the island measures
    // for them (PM_PALM_RADIUS / PM_PALM_Y) rather than at coordinates
    // typed by hand. The previous six were tuned against a different
    // island, and after the hub replaced it they would have stood on
    // ridges and in the water.
    //
    // Bearings skip the gate approaches: a palm in the middle of a path
    // reads as an obstacle on the one line the player is meant to walk.
    //
    // ── The scale is a deliberate cheat ─────────────────────────────────
    // A palm is PM_PALM_HEIGHT (388) units — a real 6 m tree — and the
    // camera orbits far enough out that true scale is about four pixels.
    // This used to carry per-tree scales of 32 to 44, which made each palm
    // taller than the island it grew on: that was the "detail looks
    // reversed" report, and it was a scale bug. 3x is an 18 m tree: still a
    // cheat, but a tree on an island rather than a tower over it.
    static const float PALM_SCALE = 3.0f;
    static const char *const PALM_OBJ[] = {
        "palm_00", "palm_01", "palm_02", "palm_03", "palm_04", "palm_05",
    };
    enum { PALM_COUNT = 10 };
    const float gate_step = 360.0f / (float)PM_GATE_COUNT;

    for (int i = 0; i < PALM_COUNT; i++) {
        float deg = 360.0f * (float)i / (float)PALM_COUNT;
        /* Nudge off any gate approach. */
        float off = deg - gate_step * (float)((int)(deg / gate_step + 0.5f));
        if (off > -12.0f && off < 12.0f) deg += 16.0f;

        const float a = deg * 0.017453293f;
        /* Alternate slightly in and out so the ring is not a stencil. */
        const float r = PM_PALM_RADIUS * ((i & 1) ? 0.94f : 1.04f);

        draw_object_at(PM_MODEL_PALMS, PALM_OBJ[i % 6],
                       (fm_vec3_t){ { fm_cosf(a) * r, PM_PALM_Y,
                                      fm_sinf(a) * r } },
                       PALM_SCALE, a);
    }

    pm_env_draw_bolt(pm_demo_scene());
    pm_env_draw_sea();
}

// ── Shot 2: the lab ────────────────────────────────────────────────────
// Where the game starts, shown before the player has been there. A slow
// push down the room toward the machine, so the attract reel answers
// "what is this game" with a place rather than a logo.
// The room is X -452..179, Y 0..160, Z -147..171 in world units — the
// dank_lab mesh's own bounding box, the same numbers pm_lab.c's collision
// brushes are built from (pm_lab.h's PM_LAB_REAL_*).
//
// The previous keys started the eye at z = 240, which is 69 units BEHIND the
// back wall. The shot opened outside the room and flew in through it, and
// since the walls are single-sided the result was geometry slicing across
// frame rather than a wall to be behind. A camera key is only meaningful
// next to the extents of what it is inside — the same lesson the flyover's
// comment above records, in the other direction.
//
// So: a push down the room's LONG axis — X spans 631 units, Z only 318 —
// from the entrance toward the moon pool and the MRI at (-330, 40, 60),
// i.e. toward LOW X. Every eye and every target sits inside the box with
// margin for the near plane, held at the room's Z midpoint rather than 0
// (dank_lab.obj's Z range is not symmetric about the origin).
// Derived from pm_lab.h's exposed extents rather than transcribed. INSET
// keeps every eye and target off the walls, so the "camera opened behind
// the back wall" bug cannot recur: if the room changes shape, the shot
// follows it.
//
// This used to push along Z using pm_world_gen.h's PM_LAB_Z0/Z1 — the
// PROCEDURAL BOX's generated dimensions, not dank_lab.obj's. The comment
// already described the real mesh and its X-long-axis correctly; the code
// was still keyed to the room nothing actually drew.
#define LAB_INSET 60.0f
#define LAB_EYE_Y (PM_LAB_REAL_Y0 + (PM_LAB_REAL_Y1 - PM_LAB_REAL_Y0) * 0.62f)
#define LAB_CENTER_Z ((PM_LAB_REAL_Z0 + PM_LAB_REAL_Z1) * 0.5f)
static PMCamKey LAB_KEYS[3];

static void lab_build_keys(void)
{
    const float x0 = PM_LAB_REAL_X0 + LAB_INSET;   // the dock/MRI end
    const float x1 = PM_LAB_REAL_X1 - LAB_INSET;   // the entrance end
    const float y  = LAB_EYE_Y;

    // A push down the room's long axis, from the entrance toward the dock.
    for (int i = 0; i < 3; i++) {
        const float f = (float)i * 0.5f;              // 0, 0.5, 1
        LAB_KEYS[i].t = 5.0f * (float)i;
        LAB_KEYS[i].eye  = (fm_vec3_t){ { x1 + (x0 - x1) * f * 0.55f, y,
                                          LAB_CENTER_Z } };
        // Always looking further down the room than the eye is, so the shot
        // reads as travelling rather than as drifting.
        LAB_KEYS[i].look = (fm_vec3_t){ { x1 + (x0 - x1) * (f * 0.55f + 0.42f),
                                          y * 0.75f, LAB_CENTER_Z } };
    }
}


static void lab_setup(void)
{
    lab_build_keys();
    pm_models_preload(PM_MODEL_LAB);
}

static void lab_draw(float elapsed)
{
    (void)elapsed;
    draw_at(PM_MODEL_LAB, (fm_vec3_t){{ 0, 0, 0 }}, 1.0f, 0.0f);
}

// ── Shot 3: the centaur ────────────────────────────────────────────────
// What you become, walking. The attract reel's closer, and the one shot
// that exists to show the rig doing its job: kiln_skel plays `walk` while
// the camera tracks alongside.
static const PMCamKey CENTAUR_KEYS[] = {
    { 0.0f, {{ 200,  90, 240 }}, {{ 0, 80,  0 }} },
    { 4.0f, {{ 120,  70, 150 }}, {{ 0, 70,  0 }} },
    { 8.0f, {{ -60,  60,  90 }}, {{ 0, 65, 10 }} },
};

static KilnSkel g_centaur_skel;
static int     g_centaur_ready;

static void centaur_setup(void)
{
    pm_models_preload(PM_MODEL_CENTAUR);
    T3DModel *centaur = pm_models_get(PM_MODEL_CENTAUR);
    if (centaur && !g_centaur_ready) {
        kiln_skel_create(&g_centaur_skel, centaur);
        kiln_skel_play(&g_centaur_skel, "walk", true);
        g_centaur_ready = 1;
    }
}

static void centaur_teardown(void)
{
    if (g_centaur_ready) {
        kiln_skel_destroy(&g_centaur_skel);
        g_centaur_ready = 0;
    }
}

static void centaur_update(float elapsed, float dt)
{
    (void)elapsed;
    if (g_centaur_ready) kiln_skel_update(&g_centaur_skel, dt);
}

static void centaur_draw(float elapsed)
{
    // Turning slowly on the spot, so eight seconds of shot shows the whole
    // silhouette without needing a locomotion path. `walk` plays throughout
    // (see centaur_setup) so the turn shows the rig moving, not just a
    // static mesh spinning.
    if (!g_centaur_ready) {
        draw_at(PM_MODEL_CENTAUR, (fm_vec3_t){{ 0, 0, 0 }}, 1.0f,
                elapsed * 0.35f);
        return;
    }

    KilnTransform *t = xform();
    t->pos = (fm_vec3_t){{ 0, 0, 0 }};
    t->scale = (fm_vec3_t){{ 1.0f, 1.0f, 1.0f }};
    t->rot_axis = (fm_vec3_t){{ 0.0f, 1.0f, 0.0f }};
    t->rot_angle = elapsed * 0.35f;
    kiln_transform_push(t);
    // Through the veil rather than kiln_skel_draw: his face, gore and armour
    // are CI4 now, so their palettes have to be bound per material. With the
    // veil down this is byte-identical output — step 0 of a ramp IS the cold
    // palette — so the reel looks exactly as it did.
    pm_veil_draw_centaur(pm_models_get(PM_MODEL_CENTAUR), &g_centaur_skel.skel);
    kiln_transform_pop();
}

// ── The opening cinematic ──────────────────────────────────────────────
// Not part of the attract reel: this one plays once, on a new profile, and
// its job is to hand the player a body.
//
// It shows Horner from outside — the only sustained look at him before the
// beach — and then drives the camera into his head, ending exactly on
// pm_lab_start_eye() looking along pm_lab_start_yaw(). pm_lab_enter picks
// up from that pose, so the cut to first person does not jump; the shot
// and the handover read as one move.
//
// The last two keys are close together on purpose. The approach is slow
// and then the final metre is quick, which is what makes it feel like
// being pulled in rather than like a camera arriving.
#define CINE_EYE_X   60.0f
#define CINE_EYE_Y  104.0f
#define CINE_EYE_Z  120.0f

// The room's real box is X -452..179, Y 0..160, Z -147..171 (pm_lab.h's
// PM_LAB_REAL_*) — dank_lab.obj's actual bounding box, now that it's what
// PM_MODEL_LAB draws. The first two keys below used to sit at
// (300, _, 330) and (150, _, 230): both well past X1/Z1 (121 and 159 units
// past, then 59 past), the same "camera opened outside the room" bug
// pm_demo's lab shot and pm_intake's LOOK keys both had, never caught here
// either because nothing that far outside a procedural box happened to
// look wrong against THAT box's very different, larger footprint.
static const PMCamKey LAB_CINE_KEYS[] = {
    // wide: him small in a room that is too big and too empty
    { 0.0f, {{ -250.0f, 150.0f,  150.0f }},
            {{ CINE_EYE_X, 70.0f, CINE_EYE_Z }} },
    // closing, still outside him
    { 4.0f, {{ 150.0f, 120.0f,  150.0f }},
            {{ CINE_EYE_X, 90.0f, CINE_EYE_Z }} },
    // at his shoulder
    { 5.4f, {{  78.0f, 108.0f,  152.0f }},
            {{ CINE_EYE_X, CINE_EYE_Y, CINE_EYE_Z - 40.0f }} },
    // and into it: the eye, looking -X, which is what pm_lab_enter expects
    { 6.0f, {{ CINE_EYE_X, CINE_EYE_Y, CINE_EYE_Z }},
            {{ CINE_EYE_X - 200.0f, CINE_EYE_Y, CINE_EYE_Z }} },
};

static KilnSkel g_lab_arms_skel;
static int     g_lab_arms_ready;

static void lab_cine_setup(void)
{
    pm_models_preload(PM_MODEL_LAB);
    pm_models_preload(PM_MODEL_HORNER);
    pm_models_preload(PM_MODEL_LAB_ARMS);

    T3DModel *arms = pm_models_get(PM_MODEL_LAB_ARMS);
    if (arms && !g_lab_arms_ready) {
        kiln_skel_create(&g_lab_arms_skel, arms);
        kiln_skel_play(&g_lab_arms_skel, "idle", true);
        g_lab_arms_ready = 1;
    }
}

static void lab_cine_teardown(void)
{
    if (g_lab_arms_ready) {
        kiln_skel_destroy(&g_lab_arms_skel);
        g_lab_arms_ready = 0;
    }
}

static void lab_cine_draw(float elapsed)
{
    draw_at(PM_MODEL_LAB, (fm_vec3_t){{ 0, 0, 0 }}, 1.0f, 0.0f);

    // He stops being drawn as the camera reaches him. Without this the
    // last half-second is spent inside his skull, looking at the back of
    // his own face — the model is not built to be seen from in there.
    if (elapsed < 5.5f) {
        // PM_HORNER_HIP_Y, not 0: his model origin is the root bone, so the
        // Y here is his hip height and 0 buries him to the waist. See
        // pm_lab.h — this call site and pm_intake.c's used to disagree by
        // exactly this constant.
        draw_at(PM_MODEL_HORNER,
                (fm_vec3_t){{ CINE_EYE_X, PM_HORNER_HIP_Y, CINE_EYE_Z }},
                1.0f, -1.5708f);
    }

    // The MRI bay's arms, idling in the background — same PM_LAB_ARMS_*
    // position pm_intake.c and pm_lab.c place them at (pm_lab.h), so the
    // hand-off between screens shows the same machine in the same place.
    if (g_lab_arms_ready) {
        KilnTransform *t = xform();
        t->pos = (fm_vec3_t){{ PM_LAB_ARMS_X, PM_LAB_ARMS_Y, PM_LAB_ARMS_Z }};
        t->scale = (fm_vec3_t){{ 1.0f, 1.0f, 1.0f }};
        t->rot_axis = (fm_vec3_t){{ 0.0f, 1.0f, 0.0f }};
        t->rot_angle = 0.0f;
        kiln_transform_push(t);
        kiln_skel_draw(&g_lab_arms_skel);
        kiln_transform_pop();
    }
}

static void lab_cine_update(float elapsed, float dt)
{
    (void)elapsed;
    if (g_lab_arms_ready) kiln_skel_update(&g_lab_arms_skel, dt);
}

const PMDemoShot pm_demo_lab_cine = {
    .name = "lab_cine", .duration = 6.0f,
    .keys = LAB_CINE_KEYS, .key_count = 4,
    .setup = lab_cine_setup, .teardown = lab_cine_teardown,
    .draw = lab_cine_draw, .update = lab_cine_update,
};

// ── The reel ───────────────────────────────────────────────────────────
// ORDER IS LOAD-BEARING: the centaur is LAST, because pm_screens locks the
// reel by shortening it by one until a profile has finished the intro.
// Adding a shot after him would unlock him by accident.
//
// Note what is NOT in here: the veil and the demons. Both are surprises
// the game springs mid-run, and an attract reel that shows them at the
// title has spent them before the player has pressed anything.
const PMDemoShot pm_demo_reel[] = {
    { .name = "flyover", .duration = 36.0f,   // must match flyover_build_keys' `dur`
      .keys = FLYOVER_KEYS, .key_count = FLYOVER_KEY_COUNT,
      .setup = flyover_setup, .draw = flyover_draw,
      // 5,870 units — ~92 m. Raised from 2,800 in lockstep with the caye's
      // land-radius resize (~2.10x): near_z/far_z are ABSOLUTE world-unit
      // constants, unlike ORBIT_RADII/ORBIT_HIGH/ORBIT_LOW above, which are
      // fractions of PM_LAND_RADIUS and rescale themselves. Left unscaled,
      // the orbit radius (which DOES scale) would end up sitting near or
      // past this far plane, clipping the camera's own foreground — the
      // same "empty grey screen" failure this file's other comments
      // describe for a mismatched geometry/frustum. The temple sits ~3,900
      // from the eye at this orbit, so it stays inside the plane and looms;
      // the far gates (~12,150) are cut, and are solid fog (pm_env.c's
      // FOG_FAR_FRAC) long before the cut. Sized AGAINST THE ORBIT: change
      // ORBIT_RADII, or resize the island again, and this has to move with
      // it — by the SAME factor as PM_LAND_RADIUS moved, or this
      // relationship silently breaks.
      .near_z = 84.0f, .far_z = 5870.0f, .exterior = 1 },
    { .name = "lab", .duration = 10.0f,
      .keys = LAB_KEYS, .key_count = 3,
      .setup = lab_setup, .draw = lab_draw },
    { .name = "centaur", .duration = 8.0f,
      .keys = CENTAUR_KEYS, .key_count = 3,
      .setup = centaur_setup, .teardown = centaur_teardown,
      .draw = centaur_draw, .update = centaur_update },
};
const int pm_demo_reel_count =
    (int)(sizeof pm_demo_reel / sizeof pm_demo_reel[0]);

// ── Director ───────────────────────────────────────────────────────────
void pm_demo_play(const PMDemoShot *shot, int loop)
{
    pm_demo_stop();
    g_shot = shot;
    g_elapsed = 0.0f;
    g_loop = loop;
    g_done = 0;
    if (shot && shot->setup) shot->setup();
}

void pm_demo_stop(void)
{
    if (g_shot && g_shot->teardown) g_shot->teardown();
    g_shot = NULL;
    g_done = 0;
}

const PMDemoShot *pm_demo_current(void) { return g_shot; }

void pm_demo_update(float dt)
{
    if (!g_shot) return;

    g_elapsed += dt;
    if (g_elapsed >= g_shot->duration) {
        if (g_loop) {
            // fmodf would do, but the duration is never zero and a while
            // loop costs nothing at one iteration.
            while (g_elapsed >= g_shot->duration)
                g_elapsed -= g_shot->duration;
        } else {
            g_elapsed = g_shot->duration;
            g_done = 1;
        }
    }
    if (g_shot->update) g_shot->update(g_elapsed, dt);
}

int   pm_demo_done(void)    { return g_done; }
float pm_demo_elapsed(void) { return g_elapsed; }

int   pm_demo_looping(void) { return g_loop; }

// ── The curve ──────────────────────────────────────────────────────────
// The bracketing search and the Catmull-Rom that used to live here moved to
// pm_camkey.h, unchanged. Not for tidiness: THREE things now have to agree
// about the curve this camera flies, and agreeing by inspection was not good
// enough for any of them.
//
//   * this, the runtime — what the player actually sees
//   * pm_debug.c's overlay, which draws the flown path next to the straight
//     line between the keys, because the gap between the two IS the overshoot
//   * pm_cine_lint, which MEASURES that gap and reports it as a number
//
// A validator measuring a curve that merely resembles the one being rendered
// is worse than no validator, because its numbers look authoritative. Sharing
// one static inline makes them identical by construction.
void pm_demo_apply(KilnCamera *cam)
{
    if (!g_shot || g_shot->key_count == 0) return;

    fm_vec3_t eye, look;
    pm_camkey_sample(g_shot->keys, g_shot->key_count, g_loop, g_elapsed,
                     &eye, &look);
    kiln_camera_set_cutscene(cam, eye, look);
}

// The scene the director last applied a frustum to. A shot's draw callback
// takes only `elapsed` — deliberately, so a shot cannot quietly start
// steering the camera — but the sky dome needs the eye position to centre
// itself on. Caching the pointer here is narrower than widening the
// callback signature for every shot that will never use it.
static KilnScene *g_scene;

const KilnScene *pm_demo_scene(void) { return g_scene; }

void pm_demo_apply_frustum(KilnScene *scene)
{
    g_scene = scene;
    if (!g_shot) return;

    // ORDER MATTERS: the frustum is applied first because pm_env_night
    // derives its fog range from scene->far_z. Lighting a scene before it
    // knows how far it can see gives fog for a different shot.
    // kiln_scene_init defaults to near 10 / far 200, which are the ENGINE's
    // units-agnostic numbers and are three metres in this world (64 units
    // to the metre — see pm_lab.h). A shot that forgot to declare its own
    // pair therefore clipped away everything it was pointed at.
    //
    // So the fallback is PM's, not the engine's. A shot still overrides it
    // when it has a reason to (the flyover needs 40,000 to reach the
    // island); it just can no longer inherit a 3 m far plane by omission.
    scene->near_z = g_shot->near_z > 0.0f ? g_shot->near_z : PM_SHOT_NEAR_Z;
    scene->far_z  = g_shot->far_z  > 0.0f ? g_shot->far_z  : PM_SHOT_FAR_Z;

    if (g_shot->exterior) {
        pm_env_night(scene);
    } else {
        // The shot's LOOK TARGET, not its eye, is what the lighting should be
        // chosen for: the interesting fixture is the one illuminating the
        // subject, and on a wide shot the eye can be most of the room away from
        // it. cam_target is already the interpolated look point for this frame
        // (pm_demo_apply wrote it), so this tracks the shot as it moves rather
        // than picking a rig once at setup.
        pm_env_interior_from_rig(scene, scene->cam_target);
    }
}

void pm_demo_draw(void)
{
    if (g_shot && g_shot->draw) g_shot->draw(g_elapsed);
}
