// SPDX-License-Identifier: MPL-2.0
//
// pm_intake.c — see pm_intake.h.

#include "pm_intake.h"

#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmodel.h>
#include <m64/m64_engine.h>

#include "pm_fx.h"
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

// 0x4000 == 90 degrees, so one binary unit is pi/32768 radians.
#define ANG (3.14159265f / 32768.0f)

// ── Sampling ───────────────────────────────────────────────────────────
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

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
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    // Smoothstep. ph_anim_intake.h is explicit about this: "smoothstep the
    // parameter - linear on a body this heavy reads mechanical."
    const float s = t * t * (3.0f - 2.0f * t);

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
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float s = t * t * (3.0f - 2.0f * t);
    return lerpf(a->dz, b->dz, s) * U;
}

// ── Camera ─────────────────────────────────────────────────────────────
// Cut to the cues, not to round numbers. Every key below sits on a beat
// the animation actually has, which is what keeps the camera feeling like
// it is watching the scene rather than running beside it.
//
// The lab's world box is X -452..179, Y 0..160, Z -147..171 and the
// machine sits at the -X end, so these look back along +X at him.
#define T(f) ((float)(f) / (float)PH_FPS)

static const PMCamKey INTAKE_KEYS[] = {
    // LOOK — wide and level, across the room. He is small and the machine
    // is not. Barely moving: the shot is him deciding.
    //
    // Eye Z was 250, then 215 — the room's real Z only runs to 171 (195
    // including wall thickness), so both sat 20-79 units behind the back
    // wall: the same "camera opened behind the wall" bug pm_demo.c's lab
    // shot comment describes, just never caught here because the model
    // being drawn until now was an unrelated procedural box. Pulled inside
    // the real Z1, keeping the same relative "further back" ordering.
    { T(CUE_LOOK),   {{ -120.0f, 110.0f, 160.0f }}, {{ -300.0f,  80.0f,  90.0f }} },
    { T(45),         {{ -150.0f, 105.0f, 150.0f }}, {{ -305.0f,  75.0f,  70.0f }} },
    // SIT — side on, low, close. The weight going down is the whole beat.
    { T(CUE_SIT),    {{ -215.0f,  55.0f, 130.0f }}, {{ -320.0f,  55.0f,  30.0f }} },
    { T(111),        {{ -235.0f,  48.0f,  95.0f }}, {{ -325.0f,  48.0f,  10.0f }} },
    // LIE — straight down at him, flat on the slab. A held overhead is the
    // one angle that makes a body look like a specimen.
    { T(CUE_LIE),    {{ -330.0f, 150.0f,  20.0f }}, {{ -330.0f,   8.0f,  16.0f }} },
    { T(CUE_MOTOR),  {{ -330.0f, 140.0f,  16.0f }}, {{ -330.0f,   8.0f,   6.0f }} },
    // MOTOR -> IN — round to the mouth of the bore and watch him go in.
    // The camera does NOT follow him inside; it stays out here, which is
    // what makes the machine feel like it took him somewhere.
    { T(170),        {{ -300.0f,  60.0f,  92.0f }}, {{ -338.0f,  30.0f,  20.0f }} },
    { T(CUE_IN),     {{ -318.0f,  42.0f,  78.0f }}, {{ -340.0f,  28.0f, -10.0f }} },
    // Held on an empty bed while the machine works.
    { T(PH_FRAMES),  {{ -322.0f,  40.0f,  74.0f }}, {{ -340.0f,  28.0f, -14.0f }} },
};

// ── Shot ───────────────────────────────────────────────────────────────
static int g_cue_motor;
static int g_cue_in;
static int g_flashed;

static void intake_setup(void)
{
    pm_models_preload(PM_MODEL_LAB);
    pm_models_preload(PM_MODEL_HORNER);
    g_cue_motor = g_cue_in = g_flashed = 0;
    pm_fx_letterbox(1.0f);
}

static void intake_teardown(void) { pm_fx_letterbox(0.0f); }

static void intake_update(float elapsed, float dt)
{
    (void)dt;
    const float frame = elapsed * (float)PH_FPS;

    // The drive starting. One sound, and a low sustained rattle that runs
    // for the rest of the shot — a machine this size does not stop once it
    // has started, and the shake is what says so.
    if (frame >= CUE_MOTOR && !g_cue_motor) {
        g_cue_motor = 1;
        pm_sfx_play(PM_SFX_MRI_START);
    }
    if (g_cue_motor && frame < CUE_IN) {
        // Re-armed every frame because pm_fx_shake decays; small enough
        // that it reads as vibration rather than as an impact.
        pm_fx_shake(3.0f, 0.5f);
    }

    if (frame >= CUE_IN && !g_cue_in) {
        g_cue_in = 1;
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

static void intake_draw(float elapsed)
{
    T3DModel *lab = pm_models_get(PM_MODEL_LAB);
    static M64Transform lab_x;
    static int lab_ready;
    if (!lab_ready) { m64_transform_init(&lab_x); lab_ready = 1; }
    if (lab) {
        lab_x.pos = (fm_vec3_t){{ 0, 0, 0 }};
        lab_x.scale = (fm_vec3_t){{ 1.0f, 1.0f, 1.0f }};
        lab_x.rot_axis = (fm_vec3_t){{ 0.0f, 1.0f, 0.0f }};
        lab_x.rot_angle = 0.0f;
        m64_transform_push(&lab_x);
        t3d_model_draw(lab);
        m64_transform_pop();
    }

    const float frame = elapsed * (float)PH_FPS;

    // Once he is inside, he is inside. Drawing him past CUE_IN puts a body
    // through the machine's own geometry, and the shot is on the empty bed
    // by then anyway.
    if (frame >= CUE_IN + 8) return;

    T3DModel *horner = pm_models_get(PM_MODEL_HORNER);
    if (!horner) return;

    float x, y, z, yaw, pitch;
    sample_root(frame, &x, &y, &z, &yaw, &pitch);

    // THE TRAP, from ph_anim_intake.h's own header: from CUE_LIE onward
    // the root translation is in the SLAB's frame, so the slab's travel
    // has to be added to it. Without this he lies still while the bed
    // leaves without him.
    if (frame >= (float)CUE_LIE) z += sample_table(frame);

    static M64Transform x_form;
    static int ready;
    if (!ready) { m64_transform_init(&x_form); ready = 1; }

    x_form.pos = (fm_vec3_t){{ x, y, z }};
    x_form.scale = (fm_vec3_t){{ 1.0f, 1.0f, 1.0f }};

    // Pass A draws him rigid, so yaw and pitch cannot both be applied
    // through M64Transform's single axis-angle. Pitch is the one that
    // carries the scene — standing to flat — and yaw only ever changes
    // once, early, while the camera is far away. So: pitch about X while
    // he is going down, yaw about Y before that.
    if (pitch < -0.01f) {
        x_form.rot_axis = (fm_vec3_t){{ 1.0f, 0.0f, 0.0f }};
        x_form.rot_angle = pitch;
    } else {
        x_form.rot_axis = (fm_vec3_t){{ 0.0f, 1.0f, 0.0f }};
        x_form.rot_angle = yaw;
    }

    m64_transform_push(&x_form);
    t3d_model_draw(horner);
    m64_transform_pop();
}

const PMDemoShot pm_intake_shot = {
    .name = "intake",
    .duration = (float)PH_FRAMES / (float)PH_FPS,  // 14.53 s
    .keys = INTAKE_KEYS,
    .key_count = (uint8_t)(sizeof INTAKE_KEYS / sizeof INTAKE_KEYS[0]),
    .setup = intake_setup, .teardown = intake_teardown,
    .draw = intake_draw, .update = intake_update,
};
