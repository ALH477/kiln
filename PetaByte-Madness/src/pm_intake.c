// SPDX-License-Identifier: MPL-2.0
//
// pm_intake.c — see pm_intake.h.

#include "pm_intake.h"

#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmodel.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_skel.h>

#include "pm_cine.h"  // the cue trace; compiles away without KILN_DEBUG
#include "pm_fx.h"
#include "pm_lab.h"
#include "pm_models.h"
#include "pm_sfx.h"

// ── The animation, in its own units ────────────────────────────────────
// Transcribed verbatim from reference/ph_anim_intake.h — same values, same
// s16 encoding, same order. Kept in source units rather than pre-converted
// to world floats so that a diff against the generated header is
// mechanical: if ph_intake_anim.py is ever re-run, these twelve rows can
// be compared line for line instead of reverse-engineered through a
// conversion. The conversion happens below, once, with named constants.
//
// Positions: s16 at 8 units per centimetre (patrick_horner_gen's --scale).
// Angles:    binary, 0x4000 == 90 degrees.
#define PH_FPS       15
#define PH_FRAMES   218

#define CUE_LOOK      0   // he is just standing there
#define CUE_SIT      66   // weight lands on the slab
#define CUE_LIE     126   // head touches down — SLAB FRAME BEGINS HERE
#define CUE_MOTOR   144   // slab drive starts
#define CUE_IN      195   // he is inside

// The new leading sequence hands off here — this file's own frame 45,
// already turned to face the machine. See pm_intake.h's "Pass B is live".
#define ORIG_HANDOFF_FRAME 45.0f

typedef struct { int16_t frame, x, y, z, yaw, pitch; } PhRoot;
typedef struct { int16_t frame, dz; } PhProp;

static const PhRoot ROOT[] = {
    {   0,  -560,  768, 1264, -32768,      0 },
    {  27,  -560,  768, 1264, -32768,      0 },
    {  45,  -560,  768, 1216,      0,      0 },  // he has turned to face it
    {  66,  -560,  776,  864,      0,      0 },  // SIT
    {  93,  -560,  776,  864,      0,      0 },
    { 111,  -560,  792,  528,      0,  -8010 },  // going back
    { 126,  -560,  800,  192,      0, -16384 },  // LIE: flat
    { 218,  -560,  800,  192,      0, -16384 },
};

static const PhProp TABLE[] = {
    {   0,     0 },
    { 144,     0 },
    { 195, -1200 },  // 1.5 m of travel into the bore
    { 218, -1200 },
};

// Source units -> world units. The mesh ships through pm_props at
// scale 0.01 (cm -> m) and lands at baseScale 64 (m -> world), so an
// 8-per-cm source unit is 64/800 of a world unit. Horner's model and this
// animation therefore agree by construction rather than by tuning.
#define U (64.0f / 800.0f)

// Generator centimetres -> world units directly (U above times the 8
// units/cm patrick_horner_gen encodes its OWN s16 header in — dank_lab_gen.py
// and ph_rig.py both work in plain cm, not that 8-per-cm encoding, so the new
// leading sequence's waypoints below use this instead of U). Matches
// pm_lab.h's PM_LAB_REAL_* derivation (world = cm * 0.01 m/cm * 64 world/m).
#define CM (U * 8.0f)

// 0x4000 == 90 degrees, so one binary unit is pi/32768 radians.
#define ANG (3.14159265f / 32768.0f)

// ── Sampling ───────────────────────────────────────────────────────────
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static float smoothstep(float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/** Root pose at `frame`, linearly interpolated and clamped at both ends. */
static void sample_root(float frame, float *x, float *y, float *z,
                        float *yaw, float *pitch)
{
    const int n = (int)(sizeof ROOT / sizeof ROOT[0]);
    int i = 0;
    while (i < n - 2 && frame >= (float)ROOT[i + 1].frame) i++;

    const PhRoot *a = &ROOT[i];
    const PhRoot *b = &ROOT[i + 1 < n ? i + 1 : i];
    const float span = (float)(b->frame - a->frame);
    float t = span > 0.0f ? (frame - (float)a->frame) / span : 0.0f;

    // Smoothstep. ph_anim_intake.h is explicit about this: "smoothstep the
    // parameter - linear on a body this heavy reads mechanical."
    const float s = smoothstep(t);

    *x = lerpf(a->x, b->x, s) * U;
    *y = lerpf(a->y, b->y, s) * U;
    *z = lerpf(a->z, b->z, s) * U;
    *yaw = lerpf(a->yaw, b->yaw, s) * ANG;
    *pitch = lerpf(a->pitch, b->pitch, s) * ANG;
}

/** Slab travel at `frame`, in world units. */
static float sample_table(float frame)
{
    const int n = (int)(sizeof TABLE / sizeof TABLE[0]);
    int i = 0;
    while (i < n - 2 && frame >= (float)TABLE[i + 1].frame) i++;

    const PhProp *a = &TABLE[i];
    const PhProp *b = &TABLE[i + 1 < n ? i + 1 : i];
    const float span = (float)(b->frame - a->frame);
    float t = span > 0.0f ? (frame - (float)a->frame) / span : 0.0f;
    return lerpf(a->dz, b->dz, smoothstep(t)) * U;
}

// ── Pass B: the new leading sequence ─────────────────────────────────────
// Distraught -> walks to the workstation -> sits -> types -> stands ->
// walks to the machine -> hands off into ROOT[]/TABLE[] above at
// ORIG_HANDOFF_FRAME. Named clips switched on state change (same
// play_once idiom pm_arrival.c uses for the centaur), one-shots gated by
// kiln_skel_is_done() rather than a guessed duration.
//
// Root position during this sequence is a plain per-state (x,z) lerp: y
// stays at standing hip height throughout (SIT_DESK's own leg bend sells
// "sitting" without the root dropping — the same thing ROOT[]'s own SIT key
// above does, barely moving y at all) and yaw stays 0 the whole time. 0 is
// the one yaw value empirically known correct here (it's what ROOT[] itself
// settles on for facing the machine from frame 45 on) — walking without
// turning to face the exact travel direction is a minor cosmetic trade
// against guessing this model's forward-axis convention for an arbitrary
// heading, which a wrong guess would show as him walking backwards.
typedef enum {
    IS_DISTRAUGHT, IS_WALK_DESK, IS_SIT_DOWN, IS_AT_DESK, IS_STAND_UP,
    IS_WALK_MRI, IS_CLIMB_IN,
} IntakeState;

#define T_DISTRAUGHT  3.0f
#define T_WALK_DESK   2.5f
#define T_SIT_DOWN    0.75f   // matches sit_down's authored 45 frames @ 60 fps
#define T_AT_DESK     3.0f
#define T_STAND_UP    0.75f   // matches stand_up's authored 45 frames @ 60 fps
#define T_WALK_MRI    3.5f
#define INTRO_T (T_DISTRAUGHT + T_WALK_DESK + T_SIT_DOWN + T_AT_DESK \
                 + T_STAND_UP + T_WALK_MRI)

// Waypoints, world units. Everything that is a fact about the ROOM now derives
// from pm_lab_gen.h (via pm_lab.h) rather than being a `cm * CM` literal — the
// generator measures the room and publishes it, so a resize moves these
// waypoints with it instead of leaving them behind. See pm_lab.h's own note.
//
// The one exception is POS_MRI_Z, flagged below: it is not room geometry.
//
// He starts at the room's FAR +Z WALL, not at the origin. dank_lab_gen.py puts
// the scanner at world (-44.8, 69.1) — only ~45-65 units from the origin — so
// "distraught, alone in a room too big and empty" was unframeable with the
// machine standing next to him. Moving the character rather than the lens is
// what fixed it. Expressed as a clearance from the measured wall so it stays
// against the wall if the room is ever resized.
#define POS_DISTRAUGHT_CLEAR (40.0f)  // a body's width and a bit off the wall
#define POS_DISTRAUGHT_X   (0.0f)
#define POS_DISTRAUGHT_Z   (PM_LAB_Z1 - POS_DISTRAUGHT_CLEAR)
#define POS_DESK_X          PM_LAB_DESK_X
#define POS_DESK_Z          PM_LAB_DESK_Z
#define POS_MRI_X           PM_MRI_X      // the scanner's own bore-axis X
// NOT derived from the scanner. This is the animation's own root Z at frame 45
// (ROOT[2].z / 8, in generator cm), transcribed with the rest of the intake
// sequence out of reference/ph_anim_intake.h — the clip was authored in its own
// space and the runtime plays it there. Deriving it from PM_MRI_Z_FACE would
// look tidier and would move him off the path his own animation walks.
#define POS_MRI_Z          (152.0f * CM)
// Standing hip height, constant throughout. PM_HORNER_HIP_Y and not
// PM_LAB_DESK_Y: this is a fact about HIS RIG (the root pivot his model is
// exported around), and it only ever matched the desk's "hip above the seat
// face" by coincidence — 61.44 against 61.40. Two numbers that happen to be
// equal are not a relationship, and moving the stool would have dropped him
// through the floor for no reason a reader could see.
#define POS_STAND_Y         PM_HORNER_HIP_Y

static KilnSkel g_skel;
static int     g_skel_ready;
static const char *g_clip;
static IntakeState g_state;
static float   g_state_t;

static KilnSkel g_arms_skel;
static int     g_arms_ready;

/** Switch clips only on a change — kiln_skel_play tears down and rebuilds
 *  the T3DAnim, so calling it every frame would restart the animation on
 *  every frame and nothing would ever visibly play. Same idiom as
 *  pm_arrival.c's play_once. */
static int play_once(const char *clip, bool loop)
{
    if (!g_skel_ready || g_clip == clip) return 0;
    // The clip name IS the cue. Every animation change in this shot is a beat
    // worth seeing on the timeline, and switching on a change is exactly the
    // edge to record it on.
    PM_CUE(clip);
    g_clip = clip;
    kiln_skel_play(&g_skel, clip, loop);
    return 1;
}

/** Root (x,z) during the leading sequence — a plain per-state lerp between
 *  named waypoints. See the file comment above on why y and yaw are held
 *  constant instead of also varying here. */
static void intro_root_xz(float *x, float *z)
{
    float x0, z0, x1, z1, t;
    switch (g_state) {
    case IS_WALK_DESK:
        x0 = POS_DISTRAUGHT_X; z0 = POS_DISTRAUGHT_Z;
        x1 = POS_DESK_X;       z1 = POS_DESK_Z;
        t = g_state_t / T_WALK_DESK;
        break;
    case IS_WALK_MRI:
        x0 = POS_DESK_X; z0 = POS_DESK_Z;
        x1 = POS_MRI_X;  z1 = POS_MRI_Z;
        t = g_state_t / T_WALK_MRI;
        break;
    case IS_SIT_DOWN: case IS_AT_DESK: case IS_STAND_UP:
        x0 = x1 = POS_DESK_X; z0 = z1 = POS_DESK_Z; t = 0.0f;
        break;
    case IS_DISTRAUGHT: default:
        x0 = x1 = POS_DISTRAUGHT_X; z0 = z1 = POS_DISTRAUGHT_Z; t = 0.0f;
        break;
    }
    const float s = smoothstep(t);
    *x = lerpf(x0, x1, s);
    *z = lerpf(z0, z1, s);
}

// ── Camera ─────────────────────────────────────────────────────────────
// Cut to the cues, not to round numbers. Every key below sits on a beat
// the animation actually has, which is what keeps the camera feeling like
// it is watching the scene rather than running beside it.
//
// The lab's world box is X -452..179, Y 0..160, Z -147..171 and the
// machine sits at the -X end, so these look back along +X at him.
//
// T() times the ORIGINAL scanner sequence, still in its own 15fps clock;
// T2() shifts that onto the WHOLE shot's clock, after the new leading
// sequence (INTRO_T seconds) and rebased off ORIG_HANDOFF_FRAME, the frame
// the leading sequence hands off at (see intake_update).
#define T(f) ((float)(f) / (float)PH_FPS)
#define T2(f) (INTRO_T + T((f) - ORIG_HANDOFF_FRAME))

static const PMCamKey INTAKE_KEYS[] = {
    // ── New: the leading sequence's own coverage ────────────────────────
    // Distraught, alone in the middle of the room — wide, so the empty
    // space around him reads as part of the beat. The scanner falls INTO
    // frame as background depth beyond him instead of standing next to him,
    // now that his own spawn has moved (see POS_DISTRAUGHT_Z above).
    //
    // These two used to sit near the +Z wall directly behind him, which is
    // the reading that matches "looking back across the room toward -Z" and
    // is not one this room can give you. He stands 40 units off a wall at
    // z=171.5, so a camera on the line from the scanner through him has at
    // most ~49 units of room before it is through the wall — and at 49 units
    // a 112-unit-tall man does not fit in an 85-degree frame. The old keys
    // were 64-88 units out with the eye at y=130-140, ABOVE his head, so
    // they looked down at him hard enough to push his feet off the bottom of
    // the frame entirely (measured: -1.24 at the second key).
    //
    // The distance has to come from somewhere, and the room is 631 units
    // wide in X against 319 deep in Z, so it comes from X — these look down
    // the room's LONG axis. That matters twice: it is the only axis with
    // enough depth to stand back on, and it puts the near walls BEHIND the
    // camera instead of in the frame. Aiming across the short axis from the
    // corner framed him correctly and still filled 40% of the picture with
    // the +X wall seventeen units from the lens.
    //
    // He lands dead centre at 40% then 47% of frame height, feet and
    // headroom both inside, with the scanner behind and to his right at
    // depth ~200 against his ~150 — the background-depth read the beat
    // wanted in the first place. Verified by projecting his own feet, hip
    // and head through these keys rather than by eye.
    { 0.0f,                    {{ 150.0f,  85.0f, 140.0f }}, {{ -110.0f, 55.0f, 120.0f }} },
    { T_DISTRAUGHT,             {{ 128.0f,  82.0f, 138.0f }}, {{ -100.0f, 54.0f, 122.0f }} },
    // Midpoint of the walk to the desk. The Catmull-Rom interpolator below
    // (pm_demo_apply/spline1) derives its tangent at a key from its TWO
    // neighbors, not just the segment it's currently in — so a big jump
    // straight from the distraught hold to the desk arrival drags the
    // tangent hard enough to overshoot the small sit/type hold that
    // follows. Splitting the jump into two steps, each the arithmetic mean
    // of its segment's endpoints (using the widened distraught key above),
    // is the standard fix for uniform-Catmull-Rom overshoot on unevenly
    // sized deltas.
    { T_DISTRAUGHT + T_WALK_DESK * 0.5f,
                                {{ 144.0f,  91.0f, 114.0f }}, {{  10.0f,  62.0f,  78.0f }} },
    // Follows him toward the console, then holds on the sit/type beat from
    // the side — a level, human framing after the wide opening.
    { T_DISTRAUGHT + T_WALK_DESK,
                                {{ 160.0f, 100.0f,  90.0f }}, {{ 120.0f,  70.0f,  34.0f }} },
    { T_DISTRAUGHT + T_WALK_DESK + T_SIT_DOWN + T_AT_DESK * 0.5f,
                                {{ 170.0f,  90.0f,  70.0f }}, {{ 118.0f,  65.0f,  34.0f }} },
    // He stands — same overshoot fix, symmetric with the key above: halves
    // the -130/-178 eye.x/look.x jump into the stand-up pull-back.
    { T_DISTRAUGHT + T_WALK_DESK + T_SIT_DOWN + T_AT_DESK,
                                {{ 105.0f, 105.0f,  65.0f }}, {{  29.0f,  72.5f,  17.0f }} },
    // Stands, then the long walk back to the machine — pulls wide again
    // so the machine is already in frame well before he arrives at it.
    { T_DISTRAUGHT + T_WALK_DESK + T_SIT_DOWN + T_AT_DESK + T_STAND_UP,
                                {{  40.0f, 120.0f,  60.0f }}, {{ -60.0f,  80.0f,   0.0f }} },
    // Midpoint of the walk back to the machine — same fix again, halving the
    // jump into the handoff key below, which is now T2(45).
    //
    // There used to be a separate `{ INTRO_T, ... }` key here, and INTRO_T is
    // EXACTLY T2(45): T2(f) is INTRO_T + T(f - ORIG_HANDOFF_FRAME) and
    // ORIG_HANDOFF_FRAME is 45, so the leading sequence's last key and the
    // original sequence's first key sat on the same instant with two different
    // poses 30 units apart. pm_camkey_sample's bracketing scan walks past a
    // zero-span segment, so the INTRO_T key was never flown THROUGH — but
    // Catmull-Rom takes its tangent from a key's two NEIGHBOURS, so it still
    // bent the curve either side of the seam while being unreachable itself.
    // Dead data that is not inert. pm_cine_lint's ERR_TIME_ORDER found it.
    //
    // Merged into T2(45), whose pose this halves toward — it keeps its own
    // "pulled inside the real Z1" history below and shifts correctly if
    // ORIG_HANDOFF_FRAME ever moves, which a literal INTRO_T would not.
    { T_DISTRAUGHT + T_WALK_DESK + T_SIT_DOWN + T_AT_DESK + T_STAND_UP + T_WALK_MRI * 0.5f,
                                {{ -55.0f, 112.5f, 105.0f }}, {{ -182.5f,  77.5f,  35.0f }} },

    // ── Original scanner sequence, shifted by INTRO_T ───────────────────
    // LOOK — wide and level, across the room. He is small and the machine
    // is not.
    //
    // Eye Z was 250, then 215 — the room's real Z only runs to 171 (195
    // including wall thickness), so both sat 20-79 units behind the back
    // wall: the same "camera opened behind the wall" bug pm_demo.c's lab
    // shot comment describes, just never caught here because the model
    // being drawn until now was an unrelated procedural box. Pulled inside
    // the real Z1, keeping the same relative "further back" ordering.
    { T2(45),        {{ -150.0f, 105.0f, 150.0f }}, {{ -305.0f,  75.0f,  70.0f }} },
    // SIT — side on, low, close. The weight going down is the whole beat.
    { T2(CUE_SIT),   {{ -215.0f,  55.0f, 130.0f }}, {{ -320.0f,  55.0f,  30.0f }} },
    { T2(111),       {{ -235.0f,  48.0f,  95.0f }}, {{ -325.0f,  48.0f,  10.0f }} },
    // LIE — straight down at him, flat on the slab. A held overhead is the
    // one angle that makes a body look like a specimen.
    { T2(CUE_LIE),   {{ -330.0f, 150.0f,  20.0f }}, {{ -330.0f,   8.0f,  16.0f }} },
    { T2(CUE_MOTOR), {{ -330.0f, 140.0f,  16.0f }}, {{ -330.0f,   8.0f,   6.0f }} },
    // MOTOR -> IN — round to the mouth of the bore and watch him go in.
    // The camera does NOT follow him inside; it stays out here, which is
    // what makes the machine feel like it took him somewhere.
    { T2(170),       {{ -300.0f,  60.0f,  92.0f }}, {{ -338.0f,  30.0f,  20.0f }} },
    { T2(CUE_IN),    {{ -318.0f,  42.0f,  78.0f }}, {{ -340.0f,  28.0f, -10.0f }} },
    // Held on an empty bed while the machine works.
    { T2(PH_FRAMES), {{ -322.0f,  40.0f,  74.0f }}, {{ -340.0f,  28.0f, -14.0f }} },
};

// ── Shot ───────────────────────────────────────────────────────────────
static int g_cue_motor;
static int g_cue_in;
static int g_flashed;

static void intake_setup(void)
{
    pm_models_preload(PM_MODEL_LAB);
    pm_models_preload(PM_MODEL_HORNER);
    pm_models_preload(PM_MODEL_LAB_ARMS);

    T3DModel *horner = pm_models_get(PM_MODEL_HORNER);
    if (horner && !g_skel_ready) {
        kiln_skel_create(&g_skel, horner);
        g_skel_ready = 1;
    }
    T3DModel *arms = pm_models_get(PM_MODEL_LAB_ARMS);
    if (arms && !g_arms_ready) {
        kiln_skel_create(&g_arms_skel, arms);
        kiln_skel_play(&g_arms_skel, "idle", true);
        g_arms_ready = 1;
    }

    g_clip = NULL;
    g_state = IS_DISTRAUGHT;
    g_state_t = 0.0f;
    g_cue_motor = g_cue_in = g_flashed = 0;
    pm_fx_letterbox(1.0f);
}

static void intake_teardown(void)
{
    pm_fx_letterbox(0.0f);
    if (g_skel_ready) { kiln_skel_destroy(&g_skel); g_skel_ready = 0; }
    if (g_arms_ready) { kiln_skel_destroy(&g_arms_skel); g_arms_ready = 0; }
}

static void intake_update(float elapsed, float dt)
{
    (void)elapsed;
    if (g_skel_ready) kiln_skel_update(&g_skel, dt);
    if (g_arms_ready) kiln_skel_update(&g_arms_skel, dt);

    if (g_state != IS_CLIMB_IN) {
        g_state_t += dt;
        switch (g_state) {
        case IS_DISTRAUGHT:
            play_once("idle_distraught", true);
            if (g_state_t >= T_DISTRAUGHT) { g_state = IS_WALK_DESK; g_state_t = 0.0f; }
            break;
        case IS_WALK_DESK:
            play_once("walk", true);
            if (g_state_t >= T_WALK_DESK) { g_state = IS_SIT_DOWN; g_state_t = 0.0f; }
            break;
        case IS_SIT_DOWN:
            play_once("sit_down", false);
            if (kiln_skel_is_done(&g_skel)) { g_state = IS_AT_DESK; g_state_t = 0.0f; }
            break;
        case IS_AT_DESK:
            play_once("sit_type", true);
            if (g_state_t >= T_AT_DESK) { g_state = IS_STAND_UP; g_state_t = 0.0f; }
            break;
        case IS_STAND_UP:
            play_once("stand_up", false);
            if (kiln_skel_is_done(&g_skel)) { g_state = IS_WALK_MRI; g_state_t = 0.0f; }
            break;
        case IS_WALK_MRI:
            play_once("walk", true);
            if (g_state_t >= T_WALK_MRI) {
                g_state = IS_CLIMB_IN;
                g_state_t = 0.0f;
                play_once("climb_in", false);
            }
            break;
        default:
            break;
        }
        return;
    }

    // ── Original scanner-sequence cues, rebased off ORIG_HANDOFF_FRAME ──
    g_state_t += dt;
    const float frame = ORIG_HANDOFF_FRAME + g_state_t * (float)PH_FPS;

    // The drive starting. One sound, and a low sustained rattle that runs
    // for the rest of the shot — a machine this size does not stop once it
    // has started, and the shake is what says so.
    if (frame >= CUE_MOTOR && !g_cue_motor) {
        g_cue_motor = 1;
        PM_CUE("cue.motor");
        pm_sfx_play(PM_SFX_MRI_START);
    }
    if (g_cue_motor && frame < CUE_IN) {
        // Re-armed every frame because pm_fx_shake decays; small enough
        // that it reads as vibration rather than as an impact.
        pm_fx_shake(3.0f, 0.5f);
    }

    if (frame >= CUE_IN && !g_cue_in) {
        g_cue_in = 1;
        // Named, because its only visible effect is a shake, and "fx.shake" on
        // its own does not say WHICH beat that was.
        PM_CUE("cue.in");
        pm_fx_shake(7.0f, 0.8f);
    }

    // The last second: the scanner runs. A white flash timed just before
    // the shot ends, so the fade to black lands on top of it and the cut
    // to the submarine happens while the frame is still blown out — the
    // player never sees the room go dark, they see it go white and then
    // they are somewhere else.
    if (frame >= PH_FRAMES - 18 && !g_flashed) {
        g_flashed = 1;
        pm_fx_flash(RGBA32(0xFF, 0xFF, 0xFF, 0xFF), 1.2f);
        pm_fx_shake(16.0f, 1.0f);
    }
}

static void draw_arms(void)
{
    if (!g_arms_ready) return;
    static KilnTransform xf;
    static int ready;
    if (!ready) { kiln_transform_init(&xf); ready = 1; }
    xf.pos = (fm_vec3_t){{ PM_LAB_ARMS_X, PM_LAB_ARMS_Y, PM_LAB_ARMS_Z }};
    xf.scale = (fm_vec3_t){{ 1.0f, 1.0f, 1.0f }};
    xf.rot_axis = (fm_vec3_t){{ 0.0f, 1.0f, 0.0f }};
    xf.rot_angle = 0.0f;
    kiln_transform_push(&xf);
    kiln_skel_draw(&g_arms_skel);
    kiln_transform_pop();
}

static void intake_draw(float elapsed)
{
    (void)elapsed;

    T3DModel *lab = pm_models_get(PM_MODEL_LAB);
    static KilnTransform lab_x;
    static int lab_ready;
    if (!lab_ready) { kiln_transform_init(&lab_x); lab_ready = 1; }
    if (lab) {
        lab_x.pos = (fm_vec3_t){{ 0, 0, 0 }};
        lab_x.scale = (fm_vec3_t){{ 1.0f, 1.0f, 1.0f }};
        lab_x.rot_axis = (fm_vec3_t){{ 0.0f, 1.0f, 0.0f }};
        lab_x.rot_angle = 0.0f;
        kiln_transform_push(&lab_x);
        t3d_model_draw(lab);
        kiln_transform_pop();
    }

    draw_arms();

    if (!g_skel_ready) return;

    float x, y, z, yaw, pitch;

    if (g_state != IS_CLIMB_IN) {
        intro_root_xz(&x, &z);
        y = POS_STAND_Y;
        yaw = 0.0f;
        pitch = 0.0f;
    } else {
        const float frame = ORIG_HANDOFF_FRAME + g_state_t * (float)PH_FPS;

        // Once he is inside, he is inside. Drawing him past CUE_IN puts a
        // body through the machine's own geometry, and the shot is on the
        // empty bed by then anyway.
        if (frame >= CUE_IN + 8) return;

        sample_root(frame, &x, &y, &z, &yaw, &pitch);

        // THE TRAP, from ph_anim_intake.h's own header: from CUE_LIE onward
        // the root translation is in the SLAB's frame, so the slab's travel
        // has to be added to it. Without this he lies still while the bed
        // leaves without him.
        if (frame >= (float)CUE_LIE) z += sample_table(frame);
    }

    static KilnTransform x_form;
    static int ready;
    if (!ready) { kiln_transform_init(&x_form); ready = 1; }

    x_form.pos = (fm_vec3_t){{ x, y, z }};
    x_form.scale = (fm_vec3_t){{ 1.0f, 1.0f, 1.0f }};

    // A skinned mesh still only gets ONE axis-angle rotation through
    // KilnTransform, same constraint Pass A had — pitch is the one that
    // carries the scanner sequence (standing to flat), yaw only ever
    // changes early, while the camera is far away, so: pitch about X while
    // he is going down, yaw about Y before that.
    if (pitch < -0.01f) {
        x_form.rot_axis = (fm_vec3_t){{ 1.0f, 0.0f, 0.0f }};
        x_form.rot_angle = pitch;
    } else {
        x_form.rot_axis = (fm_vec3_t){{ 0.0f, 1.0f, 0.0f }};
        x_form.rot_angle = yaw;
    }

    kiln_transform_push(&x_form);
    kiln_skel_draw(&g_skel);
    kiln_transform_pop();
}

const PMDemoShot pm_intake_shot = {
    .name = "intake",
    .duration = INTRO_T + (float)PH_FRAMES / (float)PH_FPS,
    .keys = INTAKE_KEYS,
    .key_count = (uint8_t)(sizeof INTAKE_KEYS / sizeof INTAKE_KEYS[0]),
    .setup = intake_setup, .teardown = intake_teardown,
    .draw = intake_draw, .update = intake_update,
};
