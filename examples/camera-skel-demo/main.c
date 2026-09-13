// SPDX-License-Identifier: MIT
//
// A goblin in a lit courtyard: walks, runs, jumps, swings a sword, taunts,
// and waves, with the camera on a spring arm behind him and an inspector that
// shows what the animation system is doing about it.
//
//   kiln_skel    -> every part of it:
//                   * Idle <-> Walk <-> Run over TWO slots, with the slots
//                     swapped at pure Walk and the incoming clip's stride phase
//                     matched to the outgoing one, so the swap is invisible;
//                   * playback rate from GROUND SPEED, so the feet stay
//                     planted: a clip's planted foot passes backwards at a
//                     measured speed (tools/blender/gait.py), and the clip plays
//                     at (his travel speed / that speed), both slots locked to one
//                     cycle rate while they blend;
//                   * Jump / Fall by crossfade, Land as a full-body overlay
//                     that hides the cut back to locomotion;
//                   * Attack and Wave on the OVERLAY slot, masked to the torso's
//                     subtree, so the legs keep running under a sword swing;
//                   * a look-at on the neck and head towards the camera;
//                   * the sword rides his right hand through a bone socket;
//                   * the feet's rise in a crouch lowers the body instead
//                     (a rotation-only rig has no root translation to do it).
//   kiln_camera  -> OoT's trailing boom, collision on
//   kiln_clip    -> the floor, walls, pillars and plinths are brushes; he
//                   slides round the pillars and can jump onto the plinths
//   kiln_audio   -> step.wav64 on each foot CONTACT in the clip, panned per foot
//   kiln_debugdraw -> the inspector's foot contacts and the socket's axes
//   kiln_input   -> camera-relative stick, and an attract tape that does it all
//
// Controls: stick walk (push further to run)   A jump   B sword
//           Z taunt   L wave   R inspector
//
// Loose DFS, not StreamDB, for the goblin: an animated model is not single-
// file. Tiny3D streams clip data from sidecar `.sdata` files via
// asset_fopen(path), a hardcoded DFS open — see CLAUDE.md's "Datafiles".
//
// Jump ROMs:
//   .#camera-skel-demo-walk  the attract tape, forced from boot
//   .#camera-skel-demo-run   a flat-out circle: Walk/Run slots, stride-synced
//   .#camera-skel-demo-jump  running and jumping
//   .#camera-skel-demo-atk   sword swings over a walk: the masked overlay
//   .#camera-skel-demo-insp  the attract tape with the inspector open
//   .#camera-skel-demo-rig   tools/gen_skel_gltf.py's hand-built 2-bone rig
//                            (idle/swing) in place of the goblin — the minimum
//                            skinned asset, kept booting

#include <libdragon.h>
#include <t3d/t3dmodel.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_camera.h>
#include <kiln/kiln_skel.h>
#include <kiln/kiln_clip.h>
#include <kiln/kiln_audio.h>
#include <kiln/kiln_prim.h>
#include <kiln/kiln_debugdraw.h>

enum { JUMP_NONE, JUMP_WALK, JUMP_RUN, JUMP_JUMP, JUMP_ATK, JUMP_INSP, JUMP_RIG };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W 320
#define SCREEN_H 240
#define PI       3.14159265f

/* goblin.py's rig stands ~2.2 m on its feet, 140 model units at baseScale 64;
 * at 0.24 he is ~34 units tall. The origin is between his feet, so the
 * transform's y IS the floor height. */
#define GOBLIN_SCALE   0.24f
#define UNITS_PER_M    (64.0f * GOBLIN_SCALE)
/* The 2-bone rig is built at baseScale 32 and is about as tall already. */
#define RIG_SCALE      0.55f

/* How fast each clip's planted foot passes backwards, in metres per second of
 * clip time — measured by tools/blender/gait.py over the shipped glTF, and held
 * to it by nix/checks/goblin-gait.nix. Playback rate = travel / this. */
#define GOBLIN_WALK_MPS 0.642f
#define GOBLIN_RUN_MPS  2.293f
/* Ankle height at rest (goblin.py's BONES: foot head at z 0.20), model units. */
#define ANKLE_REST      (0.20f * 64.0f)

#define WALK_SPEED    17.0f   /* units/s at the walk threshold  -> Walk x1.7 */
#define RUN_SPEED     46.0f   /* units/s flat out               -> Run  x1.3 */
#define RUN_STICK     0.72f   /* stick magnitude where walking becomes running */
#define ACCEL         90.0f   /* units/s^2 towards the stick's target speed    */
#define TURN_SPEED    9.0f
#define GRAVITY       340.0f
#define JUMP_VY       128.0f
#define YARD          230.0f

/* Authored facing: goblin.py puts his nose on Blender +Y, which the glTF
 * export turns into model -Z. He walks along (sin yaw, cos yaw), yaw from
 * fm_atan2f(x, z). libdragon's fm_mat4_from_axis_angle about +Y turns the
 * OTHER way from that atan2 — measured natively, R(+Y, a) maps -Z to
 * (sin a, 0, -cos a) — so the transform angle that points his nose along yaw
 * is PI - yaw, not yaw + PI. yaw + PI is right only on the Z axis and mirrors
 * him everywhere else: circling, he ran at the camera with his face to it. */
static inline float body_angle(float yaw) { return PI - yaw; }

// ── The courtyard ───────────────────────────────────────────────────────
enum { B_FLOOR, B_WALL_N, B_WALL_S, B_WALL_W, B_WALL_E };
static KilnBrush g_brushes[] = {
    [B_FLOOR]  = { .mins = {{ -YARD - 16, -16, -YARD - 16 }}, .maxs = {{ YARD + 16,  0, YARD + 16 }} },
    [B_WALL_N] = { .mins = {{ -YARD - 16,   0,  YARD      }}, .maxs = {{ YARD + 16, 44, YARD + 16 }} },
    [B_WALL_S] = { .mins = {{ -YARD - 16,   0, -YARD - 16 }}, .maxs = {{ YARD + 16, 44, -YARD     }} },
    [B_WALL_W] = { .mins = {{  YARD,        0, -YARD      }}, .maxs = {{ YARD + 16, 44, YARD      }} },
    [B_WALL_E] = { .mins = {{ -YARD - 16,   0, -YARD      }}, .maxs = {{ -YARD,     44, YARD      }} },
    /* four tall pillars, two low plinths he can jump onto */
    { .mins = {{ -122, 0,  110 }}, .maxs = {{ -98, 96,  134 }} },
    { .mins = {{   98, 0,  110 }}, .maxs = {{ 122, 96,  134 }} },
    { .mins = {{ -122, 0, -134 }}, .maxs = {{ -98, 96, -110 }} },
    { .mins = {{   98, 0, -134 }}, .maxs = {{ 122, 96, -110 }} },
    { .mins = {{ -180, 0,  -14 }}, .maxs = {{ -150, 16,  14 }} },
    { .mins = {{  150, 0,  -14 }}, .maxs = {{  180, 16,  14 }} },
};
#define BRUSH_COUNT ((int)(sizeof(g_brushes) / sizeof(g_brushes[0])))

// ── Tapes ───────────────────────────────────────────────────────────────
// Stick counts: ±85 is full tilt. A press is a key with the button, then a key
// a few frames later without it. The stick is camera-relative and the camera
// trails his heading, so a steady diagonal is a steady circle.
#define PRESS(f, b, x, y) { .frame = (f), .buttons = (b), .sx = (x), .sy = (y) }, \
                          { .frame = (f) + 4, .sx = (x), .sy = (y) }
static const KilnInputKey ATTRACT_KEYS[] = {
    { .frame =    0, .sx = 10, .sy = 42 },          /* stroll            */
    { .frame =  170, .sx = 22, .sy = 85 },          /* break into a run  */
    PRESS( 330, KILN_BTN_A, 22, 85),                /* jump at speed     */
    PRESS( 450, KILN_BTN_B, 22, 85),                /* swing, still running */
    PRESS( 520, KILN_BTN_B, 22, 85),
    PRESS( 610, KILN_BTN_A, 14, 60),
    { .frame =  720, .sx = 8, .sy = 30 },           /* ease down to a walk */
    PRESS( 800, KILN_BTN_L, 8, 30),                 /* wave while walking */
    { .frame =  930 },                              /* stop               */
    PRESS(1010, KILN_BTN_Z, 0, 0),                  /* taunt the camera   */
    { .frame = 1250 },                              /* stand: look-at     */
    { .frame = 1420 },
};
static const KilnInputTape ATTRACT = { ATTRACT_KEYS, sizeof(ATTRACT_KEYS) / sizeof(ATTRACT_KEYS[0]), 0 };

static const KilnInputKey RUN_KEYS[] = { { .frame = 0, .sx = 26, .sy = 85 } };
static const KilnInputTape RUN = { RUN_KEYS, 1, KILN_INPUT_NO_LOOP };

static const KilnInputKey JUMP_KEYS[] = {
    { .frame = 0, .sx = 20, .sy = 85 },
    PRESS(60, KILN_BTN_A, 20, 85),
    { .frame = 140 },
};
static const KilnInputTape JUMPS = { JUMP_KEYS, sizeof(JUMP_KEYS) / sizeof(JUMP_KEYS[0]), 60 };

static const KilnInputKey ATK_KEYS[] = {
    { .frame = 0, .sx = 12, .sy = 40 },
    PRESS(30, KILN_BTN_B, 12, 40),
    { .frame = 70 },
};
static const KilnInputTape ATK = { ATK_KEYS, sizeof(ATK_KEYS) / sizeof(ATK_KEYS[0]), 30 };

/* The RIG jump's circle: half tilt, so the swing blend sits mid-way. */
static const KilnInputKey CIRCLE_KEYS[] = { { .frame = 0, .sx = 20, .sy = 42 } };
static const KilnInputTape CIRCLE = { CIRCLE_KEYS, 1, KILN_INPUT_NO_LOOP };

static fm_vec3_t brush_centre(const KilnBrush *b)
{
    return (fm_vec3_t){{ (b->mins.v[0] + b->maxs.v[0]) * 0.5f,
                         (b->mins.v[1] + b->maxs.v[1]) * 0.5f,
                         (b->mins.v[2] + b->maxs.v[2]) * 0.5f }};
}

static fm_vec3_t brush_half(const KilnBrush *b)
{
    return (fm_vec3_t){{ (b->maxs.v[0] - b->mins.v[0]) * 0.5f,
                         (b->maxs.v[1] - b->mins.v[1]) * 0.5f,
                         (b->maxs.v[2] - b->mins.v[2]) * 0.5f }};
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static float wrap_pi(float a)
{
    while (a > PI) a -= 2.0f * PI;
    while (a < -PI) a += 2.0f * PI;
    return a;
}

// ── Locomotion: Idle/Walk/Run over two slots ────────────────────────────
// `gait` is one number for the whole range: 0 standing, 1 pure walk, 2 pure
// run. Below 1 the slots are Idle/Walk; above, Walk/Run.
typedef enum { LOCO_LOW, LOCO_HIGH } LocoBand;

typedef struct {
    KilnSkel *sk;
    LocoBand band;
} Loco;

static float slot_phase(const KilnSkel *sk, KilnSkelSlot s)
{
    const float len = kiln_skel_length(sk, s);
    return len > 0.0f ? kiln_skel_time(sk, s) / len : 0.0f;
}

/* Put the slots into `band`, keeping the walk's stride phase. */
static void loco_enter(Loco *lc, LocoBand band)
{
    KilnSkel *sk = lc->sk;
    float phase = 0.0f;
    const char *base = kiln_skel_clip(sk, KILN_SKEL_BASE);
    const char *blend = kiln_skel_clip(sk, KILN_SKEL_BLEND);
    if (blend && blend[0] == 'W') phase = slot_phase(sk, KILN_SKEL_BLEND);
    else if (base && base[0] == 'W') phase = slot_phase(sk, KILN_SKEL_BASE);

    if (band == LOCO_HIGH) {
        kiln_skel_play(sk, "Walk", true);
        kiln_skel_set_phase(sk, KILN_SKEL_BASE, phase);
        kiln_skel_play_blend(sk, "Run", true);
        kiln_skel_set_phase(sk, KILN_SKEL_BLEND, phase);
        kiln_skel_set_blend(sk, 0.0f);
    } else {
        kiln_skel_play(sk, "Idle", true);
        kiln_skel_play_blend(sk, "Walk", true);
        kiln_skel_set_phase(sk, KILN_SKEL_BLEND, phase);
        kiln_skel_set_blend(sk, 1.0f);
    }
    lc->band = band;
}

/* Drive the blend and both rates from `gait` and the speed he is covering. */
static void loco_drive(Loco *lc, float gait, float speed)
{
    KilnSkel *sk = lc->sk;
    if (lc->band == LOCO_LOW && gait > 1.02f) loco_enter(lc, LOCO_HIGH);
    else if (lc->band == LOCO_HIGH && gait < 0.98f) loco_enter(lc, LOCO_LOW);

    const float mps = speed / UNITS_PER_M;
    if (lc->band == LOCO_LOW) {
        kiln_skel_set_blend(sk, clampf(gait, 0.0f, 1.0f));
        kiln_skel_set_speed(sk, KILN_SKEL_BASE, 1.0f);
        /* Never quite stop the walk: at a crawl it would freeze mid-stride. */
        const float rate = mps / GOBLIN_WALK_MPS;
        kiln_skel_set_speed(sk, KILN_SKEL_BLEND, rate < 0.55f ? 0.55f : rate);
    } else {
        const float w = clampf(gait - 1.0f, 0.0f, 1.0f);
        kiln_skel_set_blend(sk, w);
        /* One cycle rate for both slots, from the distance a blended cycle
         * covers: each clip then plays at (that rate x its own length), so the
         * two stay on the same foot for the whole blend. */
        const float lw = kiln_skel_length(sk, KILN_SKEL_BASE);
        const float lr = kiln_skel_length(sk, KILN_SKEL_BLEND);
        const float per_cycle = GOBLIN_WALK_MPS * lw + (GOBLIN_RUN_MPS * lr - GOBLIN_WALK_MPS * lw) * w;
        const float cycles = mps / per_cycle;
        kiln_skel_set_speed(sk, KILN_SKEL_BASE, cycles * lw);
        kiln_skel_set_speed(sk, KILN_SKEL_BLEND, cycles * lr);
    }
}

/* Model space (model units) to world, through his transform: the same
 * rotation kiln_transform_push builds, R(+Y, a) x = (c, s), z = (-s, c). */
static fm_vec3_t model_to_world(fm_vec3_t pos, float yaw, float scale, float lift, T3DVec3 p)
{
    const float th = body_angle(yaw), c = fm_cosf(th), s = fm_sinf(th);
    return (fm_vec3_t){{ pos.v[0] + (p.v[0] * c - p.v[2] * s) * scale,
                         pos.v[1] + lift + p.v[1] * scale,
                         pos.v[2] + (p.v[0] * s + p.v[2] * c) * scale }};
}

static void slot_row(int x, int y, const char *label, const char *clip, float weight, float phase,
                     color_t ink, color_t dim, color_t on)
{
    kiln_gui_text(x, y, clip ? ink : dim, "%s %-6s", label, clip ? clip : "-");
    kiln_gui_bar(x + 66, y - 7, 34, 5, weight, on, RGBA32(0x30, 0x34, 0x44, 0xFF));
    kiln_gui_bar(x + 104, y - 7, 34, 5, phase, RGBA32(0x70, 0xB0, 0xF0, 0xFF), RGBA32(0x30, 0x34, 0x44, 0xFF));
}

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);
    asset_init_compression(2);   /* mkBlenderModel / mkModel: mkasset -c 2 */
    kiln_input_init();
    kiln_audio_init(KILN_AUDIO_DEFAULT);

    const int rig = KILN_JUMP == JUMP_RIG;
    int sfx_step = kiln_sfx_load("rom:/sfx/step.wav64");

    T3DModel *model = t3d_model_load(rig ? "rom:/models/rig.t3dm" : "rom:/models/goblin.t3dm");
    KilnSkel skel;
    kiln_skel_create(&skel, model);
    Loco loco = { .sk = &skel };
    if (rig) {
        kiln_skel_play(&skel, "idle", true);
        kiln_skel_play_blend(&skel, "swing", true);
    } else {
        loco_enter(&loco, LOCO_LOW);
    }
    const float model_scale = rig ? RIG_SCALE : GOBLIN_SCALE;

    /* Bones and masks, resolved by NAME: the exporter's order is not goblin.py's. */
    const int b_foot_l = rig ? -1 : kiln_skel_bone(&skel, "foot_l");
    const int b_foot_r = rig ? -1 : kiln_skel_bone(&skel, "foot_r");
    const int b_hand_r = rig ? -1 : kiln_skel_bone(&skel, "hand_r");
    const int b_neck = rig ? -1 : kiln_skel_bone(&skel, "neck");
    const int b_head = rig ? -1 : kiln_skel_bone(&skel, "head");
    const uint32_t upper = rig ? 0 : kiln_skel_mask_bone(&skel, "torso");

    kiln_clip_set_world(g_brushes, BRUSH_COUNT);

    static KilnPrim brush_prim[sizeof(g_brushes) / sizeof(g_brushes[0])];
    for (int i = 1; i < BRUSH_COUNT; i++) {
        const int wall = i <= B_WALL_E;
        const int plinth = g_brushes[i].maxs.v[1] < 30;
        kiln_prim_box(&brush_prim[i], brush_centre(&g_brushes[i]), brush_half(&g_brushes[i]),
                      wall ? kiln_prim_rgba(0xE0, 0xC8, 0x9C)
                           : plinth ? kiln_prim_rgba(0xF0, 0xA8, 0x60) : kiln_prim_rgba(0xD8, 0xD0, 0xC4),
                      wall ? kiln_prim_rgba(0xB0, 0x94, 0x6C)
                           : plinth ? kiln_prim_rgba(0xC0, 0x70, 0x38) : kiln_prim_rgba(0x98, 0x90, 0x8C),
                      kiln_prim_rgba(0x30, 0x28, 0x20));
    }
    KilnPrim floor_prim, shadow_prim, blade_prim, guard_prim;
    kiln_prim_floor(&floor_prim, YARD, 12,
                    kiln_prim_rgba(0xE8, 0xDC, 0xB8), kiln_prim_rgba(0xC8, 0xBC, 0x9C));
    kiln_prim_floor(&shadow_prim, 9.0f, 1,
                    kiln_prim_rgba(0x30, 0x34, 0x28), kiln_prim_rgba(0x30, 0x34, 0x28));
    /* The sword, in MODEL units (the bone matrix carries the x64; his 0.24
     * applies on top). hand_r's +Y runs out along the fingers — measured by FK:
     * straight down at rest, up and back at the wind-up, forward at the strike
     * — so the blade runs along +Y from the fist and the guard crosses it. */
    kiln_prim_box(&blade_prim, (fm_vec3_t){{ 0, 34, 0 }}, (fm_vec3_t){{ 1.4f, 24, 3.2f }},
                  kiln_prim_rgba(0xF4, 0xF6, 0xFF), kiln_prim_rgba(0xB8, 0xC4, 0xD8), kiln_prim_rgba(0x70, 0x78, 0x88));
    kiln_prim_box(&guard_prim, (fm_vec3_t){{ 0, 9, 0 }}, (fm_vec3_t){{ 2.0f, 1.6f, 8.0f }},
                  kiln_prim_rgba(0xF0, 0xC0, 0x40), kiln_prim_rgba(0xB0, 0x80, 0x20), kiln_prim_rgba(0x60, 0x40, 0x10));

    KilnScene scene;
    kiln_scene_init(&scene);
    kiln_prim_stage(&scene, RGBA32(0x5C, 0x70, 0x90, 0xFF), 260.0f, 620.0f);
    scene.fov_deg = 60.0f;
    scene.near_z = 6.0f;
    scene.far_z = 640.0f;

    /* Close enough that he fills a third of the frame: the boom used to sit at
     * 96 back, where a 34-unit goblin was a figure in the distance. */
    KilnCamera cam;
    kiln_camera_init(&cam);
    cam.distance = 66.0f;
    cam.height = 30.0f;
    cam.look_height = 22.0f;
    kiln_camera_set_collision(&cam, 1);

    const fm_vec3_t mins = {{ -7, 1, -7 }};    /* sideways: bottom clear of the floor */
    const fm_vec3_t vmins = {{ -7, 0, -7 }};   /* downwards: the feet themselves      */
    const fm_vec3_t maxs = {{  7, 32,  7 }};
    fm_vec3_t pos = {{ 0, 0.05f, 8 }};
    float yaw = 0.0f, vy = 0.0f, speed = 0.0f, lift = 0.0f, look_w = 0.0f;
    int grounded = 1, air_state = 0;  /* 0 ground, 1 rising, 2 falling */
    int inspector = KILN_JUMP == JUMP_INSP;
    kiln_camera_snap(&cam, pos, yaw);

    if (KILN_JUMP == JUMP_WALK || KILN_JUMP == JUMP_INSP) kiln_input_play(1, &ATTRACT);
    else if (KILN_JUMP == JUMP_RUN) kiln_input_play(1, &RUN);
    else if (KILN_JUMP == JUMP_JUMP) kiln_input_play(1, &JUMPS);
    else if (KILN_JUMP == JUMP_ATK) kiln_input_play(1, &ATK);
    else if (rig) kiln_input_play(1, &CIRCLE);
    else kiln_input_set_attract(1, &ATTRACT, 360);

    KilnTransform body_xf, shadow_xf;
    kiln_transform_init(&body_xf);
    kiln_transform_init(&shadow_xf);

    float last_phase = 0.0f;
    int foot = 0;
    uint32_t frames = 0;
    float fps = 60.0f;
    uint32_t last_ticks = get_ticks();
    const char *action = "";

    for (;;) {
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);
        const float dt = 1.0f / 60.0f;

        if (kiln_input_pressed(1, KILN_BTN_R)) inspector = !inspector;

        const int taunting = !rig && kiln_skel_overlay_active(&skel) &&
                             kiln_skel_clip(&skel, KILN_SKEL_OVERLAY)[0] == 'T';

        /* Camera-relative basis. Looking down +Z, screen-right is -X. */
        fm_vec3_t fwd = {{ cam.look.v[0] - cam.eye.v[0], 0, cam.look.v[2] - cam.eye.v[2] }};
        fm_vec3_norm(&fwd, &fwd);
        const fm_vec3_t right = {{ -fwd.v[2], 0, fwd.v[0] }};
        fm_vec3_t dir = {{ fwd.v[0] * in->stick_y + right.v[0] * in->stick_x, 0,
                           fwd.v[2] * in->stick_y + right.v[2] * in->stick_x }};
        float mag = fm_vec3_len(&dir);
        if (mag > 1.0f) mag = 1.0f;
        if (taunting) mag = 0.0f;

        /* Target speed: up to WALK_SPEED over the first RUN_STICK of the
         * stick, then on to RUN_SPEED. */
        const float target = mag < RUN_STICK ? WALK_SPEED * mag / RUN_STICK
                                             : WALK_SPEED + (RUN_SPEED - WALK_SPEED) * (mag - RUN_STICK) / (1.0f - RUN_STICK);
        speed += clampf(target - speed, -ACCEL * 1.6f * dt, ACCEL * dt);

        fm_vec3_t moved = pos;
        if (mag > 0.01f) {
            const float t = TURN_SPEED * dt;
            yaw = fm_lerp_angle(yaw, fm_atan2f(dir.v[0], dir.v[2]), t > 1.0f ? 1.0f : t);
        }
        if (speed > 0.01f) {
            /* Along his facing, not the stick: a turn arcs instead of strafing. */
            const fm_vec3_t disp = {{ fm_sinf(yaw) * speed * dt, 0, fm_cosf(yaw) * speed * dt }};
            moved = kiln_clip_slide(pos, disp, mins, maxs, 4);
        }
        fm_vec3_t delta = {{ moved.v[0] - pos.v[0], 0, moved.v[2] - pos.v[2] }};
        const float covered = fm_vec3_len(&delta) / dt;   /* walking into a pillar is standing still */
        pos = moved;

        /* ── Vertical ─────────────────────────────────────────────────── */
        if (!rig && grounded && kiln_input_pressed(1, KILN_BTN_A) && !taunting) {
            vy = JUMP_VY;
            grounded = 0;
            air_state = 1;
            kiln_skel_set_speed(&skel, KILN_SKEL_BASE, 1.0f);
            kiln_skel_set_speed(&skel, KILN_SKEL_BLEND, 1.0f);
            kiln_skel_crossfade(&skel, "Jump", false, 0.08f);
            action = "jump";
        }
        vy -= GRAVITY * dt;
        {
            const float dy = vy * dt;
            const fm_vec3_t down = {{ 0, dy, 0 }};
            const fm_vec3_t after = kiln_clip_slide(pos, down, vmins, maxs, 1);
            const int blocked = after.v[1] > pos.v[1] + dy + 1e-3f;
            pos = after;
            if (blocked && vy < 0.0f) {
                if (!grounded && !rig) {
                    /* Touchdown: straight back into locomotion, with Land over
                     * the whole body to cover the cut. */
                    loco_enter(&loco, speed > WALK_SPEED ? LOCO_HIGH : LOCO_LOW);
                    kiln_skel_set_overlay_mask(&skel, KILN_POSE_MASK_ALL);
                    kiln_skel_overlay(&skel, "Land", false, 0.06f);
                    action = "land";
                }
                grounded = 1;
                air_state = 0;
                vy = 0.0f;
            } else if (blocked) {
                vy = 0.0f;     /* head hit something */
            } else if (vy < -40.0f) {
                grounded = 0;
            }
        }
        if (!rig && !grounded && air_state == 1 && vy < 0.0f) {
            kiln_skel_crossfade(&skel, "Fall", true, 0.25f);
            air_state = 2;
        }
        if (!rig && !grounded && air_state == 0) {
            /* Walked off a plinth. */
            kiln_skel_set_speed(&skel, KILN_SKEL_BASE, 1.0f);
            kiln_skel_set_speed(&skel, KILN_SKEL_BLEND, 1.0f);
            kiln_skel_crossfade(&skel, "Fall", true, 0.2f);
            air_state = 2;
        }

        /* ── Overlays ─────────────────────────────────────────────────── */
        if (!rig) {
            const int busy = kiln_skel_overlay_active(&skel);
            if (kiln_input_pressed(1, KILN_BTN_B)) {
                kiln_skel_set_overlay_mask(&skel, upper);
                kiln_skel_set_speed(&skel, KILN_SKEL_OVERLAY, 1.2f);
                kiln_skel_overlay(&skel, "Attack", false, 0.05f);
                action = "sword";
            } else if (!busy && kiln_input_pressed(1, KILN_BTN_L)) {
                kiln_skel_set_overlay_mask(&skel, upper);
                kiln_skel_set_speed(&skel, KILN_SKEL_OVERLAY, 1.0f);
                kiln_skel_overlay(&skel, "Wave", false, 0.18f);
                action = "wave";
            } else if (!busy && grounded && kiln_input_pressed(1, KILN_BTN_Z)) {
                kiln_skel_set_overlay_mask(&skel, KILN_POSE_MASK_ALL);
                kiln_skel_set_speed(&skel, KILN_SKEL_OVERLAY, 1.0f);
                kiln_skel_overlay(&skel, "Taunt", false, 0.2f);
                action = "taunt";
            }
        }

        /* ── Locomotion ───────────────────────────────────────────────── */
        const float gait = covered <= WALK_SPEED ? covered / WALK_SPEED
                                                 : 1.0f + (covered - WALK_SPEED) / (RUN_SPEED - WALK_SPEED);
        if (rig) {
            kiln_skel_set_blend(&skel, clampf(gait, 0.0f, 1.0f));
        } else if (grounded) {
            loco_drive(&loco, gait, covered);
        }

        /* Look at the camera when standing still with nothing else to do. */
        if (!rig) {
            const int idle = grounded && gait < 0.15f && !kiln_skel_overlay_active(&skel);
            look_w += ((idle ? 1.0f : 0.0f) - look_w) * 0.06f;
            if (look_w > 0.01f) {
                const float to_cam = fm_atan2f(cam.eye.v[0] - pos.v[0], cam.eye.v[2] - pos.v[2]);
                const float d = clampf(wrap_pi(to_cam - yaw), -1.0f, 1.0f) * look_w;
                T3DQuat q;
                kiln_quat_axis_angle(&q, 0, 1, 0, d * 0.4f);
                kiln_skel_bone_rotate(&skel, b_neck, &q);
                kiln_quat_axis_angle(&q, 0, 1, 0, d * 0.6f);
                kiln_skel_bone_rotate(&skel, b_head, &q);
            }
        }

        kiln_skel_update(&skel, dt);

        /* ── Feet: the crouch lowers the body, contacts make the sound ── */
        fm_vec3_t foot_w[2] = { pos, pos };
        if (!rig) {
            const T3DVec3 fl = kiln_skel_bone_pos(&skel, b_foot_l), fr = kiln_skel_bone_pos(&skel, b_foot_r);
            const float rise = (fl.v[1] < fr.v[1] ? fl.v[1] : fr.v[1]) - ANKLE_REST;
            const float want = grounded && rise > 0.0f ? -rise * model_scale : 0.0f;
            lift += (want - lift) * 0.5f;
            foot_w[0] = model_to_world(pos, yaw, model_scale, lift, fl);
            foot_w[1] = model_to_world(pos, yaw, model_scale, lift, fr);

            /* Walk contacts at phase 0 and 1/2; Run's are at the same phases,
             * and the two slots share a cycle rate, so one reading serves both. */
            KilnSkelSlot ws = loco.band == LOCO_LOW ? KILN_SKEL_BLEND : KILN_SKEL_BASE;
            const char *wc = kiln_skel_clip(&skel, ws);
            if (grounded && wc && wc[0] == 'W' && gait > 0.25f) {
                const float ph = slot_phase(&skel, ws);
                const int crossed = (last_phase < 0.5f && ph >= 0.5f) || ph < last_phase;
                if (crossed && sfx_step >= 0) {
                    foot = !foot;
                    kiln_sfx_play_ex(sfx_step, -1, 1, 0.35f + 0.25f * clampf(gait, 0, 2), foot ? 0.40f : 0.60f);
                }
                last_phase = ph;
            }
        } else {
            /* The rig has no feet to listen to: a footfall every 13 units. */
            static float stride = 0.0f;
            stride += covered * dt;
            if (stride >= 13.0f && sfx_step >= 0) {
                stride -= 13.0f;
                foot = !foot;
                kiln_sfx_play_ex(sfx_step, -1, 1, 0.5f, foot ? 0.42f : 0.58f);
            }
        }

        kiln_camera_update(&cam, pos, yaw, dt);
        kiln_camera_apply(&cam, &scene);
        kiln_scene_update(&scene);

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        /* ── 3D pass ─────────────────────────────────────────────────── */
        kiln_frame_begin();
        kiln_scene_begin(&scene);

        kiln_prim_draw(&floor_prim);
        for (int i = 1; i < BRUSH_COUNT; i++) kiln_prim_draw(&brush_prim[i]);

        /* The shadow stays on whatever he would land on: the floor, or a plinth. */
        {
            float ground_y = 0.0f;
            for (int i = B_WALL_E + 1; i < BRUSH_COUNT; i++) {
                const KilnBrush *b = &g_brushes[i];
                if (pos.v[0] > b->mins.v[0] && pos.v[0] < b->maxs.v[0] && pos.v[2] > b->mins.v[2] &&
                    pos.v[2] < b->maxs.v[2] && b->maxs.v[1] <= pos.v[1] + 0.5f && b->maxs.v[1] > ground_y)
                    ground_y = b->maxs.v[1];
            }
            const float h = clampf(1.0f - (pos.v[1] - ground_y) / 60.0f, 0.4f, 1.0f);
            shadow_xf.pos = (fm_vec3_t){{ pos.v[0], ground_y + 0.5f, pos.v[2] }};
            shadow_xf.scale = (fm_vec3_t){{ h, 1, h }};
            kiln_transform_push(&shadow_xf);
            kiln_prim_draw(&shadow_prim);
            kiln_transform_pop();
        }

        body_xf.pos = (fm_vec3_t){{ pos.v[0], pos.v[1] + lift, pos.v[2] }};
        body_xf.scale = (fm_vec3_t){{ model_scale, model_scale, model_scale }};
        body_xf.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
        body_xf.rot_angle = body_angle(yaw);
        kiln_transform_push(&body_xf);
        kiln_skel_draw(&skel);
        if (!rig) {
            kiln_skel_bone_push(&skel, b_hand_r);
            kiln_prim_draw(&guard_prim);
            kiln_prim_draw(&blade_prim);
            kiln_transform_pop();
        }
        kiln_transform_pop();

        /* ── 2D pass ─────────────────────────────────────────────────── */
        kiln_gui_begin();
        const color_t ink = RGBA32(0xE8, 0xE8, 0xF0, 0xFF);
        const color_t gold = RGBA32(0xFF, 0xC8, 0x60, 0xFF);
        const color_t dim = RGBA32(0x98, 0xA0, 0xB4, 0xFF);
        const color_t green = RGBA32(0x60, 0xE0, 0x80, 0xFF);
        const color_t panel = RGBA32(0x14, 0x18, 0x24, 0xFF);

        if (inspector && !rig) {
            kiln_dd_begin(&scene, SCREEN_W, SCREEN_H);
            const int planted = foot_w[0].v[1] < foot_w[1].v[1] ? 0 : 1;
            for (int f = 0; f < 2; f++) {
                const color_t c = grounded && f == planted ? green : RGBA32(0xF0, 0x70, 0x60, 0xFF);
                kiln_dd_point(foot_w[f], 3, c);
                kiln_dd_line(foot_w[f], (fm_vec3_t){{ foot_w[f].v[0], pos.v[1], foot_w[f].v[2] }}, c);
            }
            kiln_dd_axes(model_to_world(pos, yaw, model_scale, lift, kiln_skel_bone_pos(&skel, b_hand_r)), 8.0f);
            kiln_dd_end();

            const int px = 8, py = 8;
            kiln_gui_panel(px, py, 152, 82, panel, gold);
            kiln_gui_text(px + 6, py + 12, gold, "KILN_SKEL  %s", loco.band == LOCO_HIGH ? "walk-run" : "idle-walk");
            const float bw = skel.blend_factor;
            const float ow = skel.overlay_weight;
            slot_row(px + 6, py + 26, "B", kiln_skel_clip(&skel, KILN_SKEL_BASE), 1.0f - bw,
                     slot_phase(&skel, KILN_SKEL_BASE), ink, dim, gold);
            slot_row(px + 6, py + 38, "L", kiln_skel_clip(&skel, KILN_SKEL_BLEND), bw,
                     slot_phase(&skel, KILN_SKEL_BLEND), ink, dim, gold);
            slot_row(px + 6, py + 50, "O", kiln_skel_clip(&skel, KILN_SKEL_OVERLAY), ow,
                     slot_phase(&skel, KILN_SKEL_OVERLAY), ink, dim, RGBA32(0xF0, 0x80, 0xC0, 0xFF));
            kiln_gui_text(px + 6, py + 64, ink, "x%.2f x%.2f %4.1fm/s",
                          (double)skel.slot_speed[KILN_SKEL_BASE], (double)skel.slot_speed[KILN_SKEL_BLEND],
                          (double)(covered / UNITS_PER_M));
            kiln_gui_text(px + 6, py + 74, dim, "mask %s  lift %.1f", ow > 0.0f && skel.overlay_mask != KILN_POSE_MASK_ALL
                          ? "torso" : "all", (double)lift);
        } else {
            kiln_gui_panel(8, 8, 142, 42, panel, gold);
            kiln_gui_text(14, 21, gold, rig ? "KILN SKEL - RIG" : "KILN COURTYARD");
            kiln_gui_text(14, 33, ink, "%-5s %s", rig ? (gait > 0.5f ? "swing" : "idle")
                          : !grounded ? "air" : gait > 1.0f ? "run" : gait > 0.15f ? "walk" : "idle", action);
            kiln_gui_text(14, 45, dim, "%4.1f fps", (double)fps);
        }

        if (kiln_input_scripted(1)) {
            kiln_gui_panel(SCREEN_W - 58, 8, 50, 16, RGBA32(0xC0, 0x30, 0x60, 0xFF),
                           RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
            kiln_gui_text(SCREEN_W - 49, 20, RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "DEMO");
        }

        kiln_gui_panel(8, SCREEN_H - 24, SCREEN_W - 16, 16, panel, RGBA32(0x60, 0x90, 0xC0, 0xFF));
        kiln_gui_text(14, SCREEN_H - 12, ink, rig ? "stick walk (camera-relative)"
                                                  : "A jump B sword Z taunt L wave R inspect");

        kiln_gui_end();
        kiln_frame_end();
        kiln_audio_update();
    }
}
