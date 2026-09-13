// SPDX-License-Identifier: MIT
//
// The OoT + id Tech 4 integration proof: a small temple room from a Quake .map,
// a swordsman driven by kiln_player, two creatures to fight, Z-targeting, and a
// camera that opens with an orbit and then follows like Ocarina's.
//
//   kiln_skel     -> the goblin hero: Idle/Run crossfades at a stride-matched rate,
//                    Jump/Fall, a Land overlay, Roll spun about its pivot, the
//                    sword swing masked to the torso, a head that tracks the
//                    Z-target, and a sword and shield riding his hands
//   kiln_map      -> assets/oot_test.map -> brushes, faces, spawns; kiln_map_tint
//   kiln_dict     -> spawn args (origin, angle) read at spawn
//   kiln_clip     -> swept-AABB vs the parsed brushes plus two torch pillars
//   kiln_player   -> IDLE/WALK/RUN/ROLL/ATTACK/JUMP/FALL + FOOTSTEP events
//   kiln_event    -> deferred FOOTSTEP dispatch
//   kiln_target   -> cone acquire + reticle projection
//   kiln_camera   -> mode stack (CUTSCENE -> NORMAL <-> TARGETING), collision boom
//   kiln_surface  -> friction table (index 0 = stone)
//   kiln_sound    -> footstep and sword-hit shaders
//
//   stick move   Z target   A sword   B jump   L roll   R run
//   idle 3 s: the demo locks on, closes in and fights
//
// Jump ROMs: .#oot-demo-target boots beside a creature with Z held, so the
// TARGETING camera and reticle are on screen with no controller;
// .#oot-demo-attack does the same and keeps swinging; .#oot-demo-roll runs and
// dodge-rolls.
//
// The host builds (pc-oot-demo, pc-oot-demo-target) set KILN_OOT_PRIM_BODY and
// keep the five-box hero: plat/host cannot run a skeleton, and a host that
// silently drew nothing would be worse than one drawing boxes.

#include <libdragon.h>
#include <t3d/t3dmodel.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_clip.h>
#include <kiln/kiln_dict.h>
#include <kiln/kiln_map.h>
#include <kiln/kiln_actor.h>
#include <kiln/kiln_event.h>
#include <kiln/kiln_player.h>
#include <kiln/kiln_target.h>
#include <kiln/kiln_camera.h>
#include <kiln/kiln_surface.h>
#include <kiln/kiln_sound.h>
#include <kiln/kiln_audio.h>
#include <kiln/kiln_prim.h>
#include <kiln/kiln_debugdraw.h>
#include <kiln/kiln_skel.h>
#ifdef KILN_DEBUG
#include <kiln/kiln_console.h>
#include <kiln/kiln_prof.h>
#endif

enum { JUMP_NONE, JUMP_TARGET, JUMP_ROLL, JUMP_ATTACK };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif
#ifndef KILN_OOT_PRIM_BODY
#define KILN_OOT_PRIM_BODY 0
#endif

#define SCREEN_W 320
#define SCREEN_H 240
#define ACTOR_POOL_CAP 16
#define ENEMY_HP 3
#define ATTACK_TIME 0.4f   /* kiln_player.c's ATTACK_TIME; the swing is drawn over it */
#define ROLL_TIME   0.45f  /* kiln_player.c's ROLL_TIME                               */
#define PI          3.14159265f

/* The goblin hero. kiln_player's box is 32 tall; goblin.py's rig is 2.19 m at
 * 64 units a metre, so 0.30 stands him a little over it — head above the box,
 * which reads better than a goblin shrunk to fit a collision volume. */
#define GOBLIN_SCALE        0.30f
#define UNITS_PER_M         (64.0f * GOBLIN_SCALE)
/* Measured by tools/blender/gait.py, held to it by nix/checks/goblin-gait.nix. */
#define GOBLIN_RUN_MPS      2.293f
#define GOBLIN_ROLL_PIVOT_M 0.45f
#define ANKLE_REST          (0.20f * 64.0f)
/* Clip lengths, seconds at goblin.py's 24 fps: the attack and roll windows
 * kiln_player enforces are shorter, so the clips play faster to fit. */
#define ATTACK_CLIP_S       0.5f
#define ROLL_CLIP_S         (14.0f / 24.0f)

enum { PROFILE_PLAYER, PROFILE_ENEMY, PROFILE_COUNT };

// ── Geometry ────────────────────────────────────────────────────────────
static KilnPrim g_tunic, g_head, g_cap, g_shield, g_sword;
static KilnPrim g_enemy_body, g_enemy_flash, g_enemy_eye, g_shadow;
static KilnPrim g_pillar, g_flame;
static KilnTransform g_sword_xf, g_eye_xf[2], g_shadow_xf, g_deco_xf;

/* The skinned hero, console only (see KILN_OOT_PRIM_BODY). */
static T3DModel *g_goblin;
static KilnSkel g_skel;
static KilnPrim g_blade, g_guard, g_buckler;
static KilnTransform g_pivot_xf, g_body_xf;
static int g_b_hand_r = -1, g_b_hand_l = -1, g_b_foot_l = -1, g_b_foot_r = -1, g_b_neck = -1, g_b_head = -1;
static uint32_t g_upper;
typedef struct {
    int state;           /* the KilnPlayerState last animated         */
    fm_vec3_t prev;      /* for the speed actually covered            */
    float lift;          /* the body lowered by the feet's rise       */
    float look_w;
} Hero;
static Hero g_hero = { .state = -1 };
/* Defined with the other globals below; the hero's head tracks it. */
static KilnActorHandle g_lock_h;

/* The map's brushes plus two torch pillars, as one clip world. */
static KilnBrush g_world[32];
static uint16_t g_world_count;
static const fm_vec3_t PILLAR_AT[2] = { {{ 64, 0, -64 }}, {{ -64, 0, 64 }} };

// ── Enemy ──────────────────────────────────────────────────────────────
typedef struct {
    float angle;
    fm_vec3_t centre;
    int hp;
    float flash, hop, hop_v, dead_t;
} EnemyState;

static void enemy_init(KilnActor *self, const KilnDict *spawn_args)
{
    (void)spawn_args;
    EnemyState *s = (EnemyState *)self->state;
    s->centre = self->xform.pos;    /* kiln_map wrote the "origin" epair here */
    s->angle = self->xform.pos.v[0] > 0 ? 0.0f : 3.1f;
    s->hp = ENEMY_HP;
    self->xform.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
}

static void enemy_update(KilnActor *self, float dt)
{
    EnemyState *s = (EnemyState *)self->state;
    if (s->dead_t > 0.0f) {
        s->dead_t -= dt;
        if (s->dead_t <= 0.0f) { s->hp = ENEMY_HP; s->hop = 0; s->hop_v = 120.0f; }
    }
    s->angle += 0.9f * dt;
    /* Orbit the spawn point itself. The old version added an offset to the
     * CURRENT position each frame, so each enemy circled a point ~45 units
     * from where it spawned. */
    self->xform.pos.v[0] = s->centre.v[0] + fm_cosf(s->angle) * 22.0f;
    self->xform.pos.v[2] = s->centre.v[2] + fm_sinf(s->angle) * 22.0f;
    s->hop_v -= 400.0f * dt;
    s->hop += s->hop_v * dt;
    if (s->hop < 0) { s->hop = 0; s->hop_v = 0; }
    self->xform.pos.v[1] = s->centre.v[1] + s->hop + 2.0f * fm_sinf(s->angle * 5.0f);
    /* Face along the orbit: the tangent of (cos a, sin a) is (-sin a, cos a).
     * libdragon's axis-angle about +Y maps +Z to (-sin t, cos t), so the
     * angle that points the eyes (+Z) along that tangent is `a` itself; the
     * atan2 this used to take mirrored them off the Z axis. */
    self->xform.rot_angle = s->angle;
    /* Squash and stretch: long on the way up, flat on landing, and a shudder
     * for the length of a hit flash. */
    const float k = s->hop_v / 900.0f;
    const float shake = s->flash > 0 ? 0.12f * fm_sinf(s->flash * 60.0f) : 0.0f;
    const float sy = 1.0f + (k > 0.25f ? 0.25f : (k < -0.2f ? -0.2f : k)) + shake;
    const float sxz = 1.0f / (sy > 0.5f ? sy : 0.5f);
    self->xform.scale = (fm_vec3_t){{ sxz, sy, sxz }};
    if (s->flash > 0) s->flash -= dt;
}

static void enemy_draw(KilnActor *self)
{
    const EnemyState *s = (const EnemyState *)self->state;
    if (s->dead_t > 0.0f) return;
    kiln_prim_draw(s->flash > 0 ? &g_enemy_flash : &g_enemy_body);
    for (int i = 0; i < 2; i++) {
        kiln_transform_push(&g_eye_xf[i]);
        kiln_prim_draw(&g_enemy_eye);
        kiln_transform_pop();
    }
}

// ── Player ─────────────────────────────────────────────────────────────
static void player_init(KilnActor *self, const KilnDict *spawn_args)
{
    (void)spawn_args;
    KilnPlayer *p = kiln_player_of(self);
    *p = (KilnPlayer){ .state = KILN_PLAYER_IDLE, .on_ground = 1 };
    self->xform.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static float wrap_pi(float a)
{
    while (a > PI) a -= 2.0f * PI;
    while (a < -PI) a += 2.0f * PI;
    return a;
}

/* Which of BASE/BLEND is playing `clip`, or -1. */
static int slot_of(const char *clip)
{
    for (int s = KILN_SKEL_BASE; s <= KILN_SKEL_BLEND; s++) {
        const char *c = kiln_skel_clip(&g_skel, (KilnSkelSlot)s);
        if (c && c[0] == clip[0] && c[1] == clip[1]) return s;
    }
    return -1;
}

/* kiln_player owns the state; this turns each state into animation. */
static void hero_animate(KilnActor *self, const KilnPlayer *p, float dt)
{
    const int st = (int)p->state;
    const int air = st == KILN_PLAYER_JUMP || st == KILN_PLAYER_FALL;
    const int was_air = g_hero.state == KILN_PLAYER_JUMP || g_hero.state == KILN_PLAYER_FALL;
    fm_vec3_t d = {{ self->xform.pos.v[0] - g_hero.prev.v[0], 0, self->xform.pos.v[2] - g_hero.prev.v[2] }};
    const float speed = g_hero.state < 0 ? 0.0f : fm_vec3_len(&d) / dt;
    g_hero.prev = self->xform.pos;

    if (st != g_hero.state) {
        switch (st) {
        case KILN_PLAYER_ROLL:
            kiln_skel_set_speed(&g_skel, KILN_SKEL_BASE, ROLL_CLIP_S / ROLL_TIME);
            kiln_skel_set_speed(&g_skel, KILN_SKEL_BLEND, ROLL_CLIP_S / ROLL_TIME);
            kiln_skel_crossfade(&g_skel, "Roll", false, 0.05f);
            break;
        case KILN_PLAYER_JUMP:
            kiln_skel_set_speed(&g_skel, KILN_SKEL_BASE, 1.0f);
            kiln_skel_set_speed(&g_skel, KILN_SKEL_BLEND, 1.0f);
            kiln_skel_crossfade(&g_skel, "Jump", false, 0.06f);
            break;
        case KILN_PLAYER_FALL:
            kiln_skel_set_speed(&g_skel, KILN_SKEL_BASE, 1.0f);
            kiln_skel_set_speed(&g_skel, KILN_SKEL_BLEND, 1.0f);
            kiln_skel_crossfade(&g_skel, "Fall", true, 0.2f);
            break;
        case KILN_PLAYER_ATTACK:
            /* Masked to the torso: the legs keep whatever they were doing. */
            kiln_skel_set_overlay_mask(&g_skel, g_upper);
            kiln_skel_set_speed(&g_skel, KILN_SKEL_OVERLAY, ATTACK_CLIP_S / ATTACK_TIME);
            kiln_skel_overlay(&g_skel, "Attack", false, 0.04f);
            break;
        default:
            if (was_air) {
                kiln_skel_set_overlay_mask(&g_skel, KILN_POSE_MASK_ALL);
                kiln_skel_set_speed(&g_skel, KILN_SKEL_OVERLAY, 1.2f);
                kiln_skel_overlay(&g_skel, "Land", false, 0.05f);
            }
            break;
        }
        g_hero.state = st;
    }

    /* Grounded: Idle or Run, at the rate that keeps a planted foot planted.
     * kiln_player's WALK is 60 units/s, which the goblin's own Walk clip could
     * only match at 4.2x, so both of its moving states use Run (1.4x at a
     * walk, 3.2x flat out) — a jog and a sprint. */
    if (st == KILN_PLAYER_IDLE || st == KILN_PLAYER_WALK || st == KILN_PLAYER_RUN || st == KILN_PLAYER_ATTACK) {
        const int moving = (st == KILN_PLAYER_WALK || st == KILN_PLAYER_RUN) && speed > 1.0f;
        kiln_skel_crossfade(&g_skel, moving ? "Run" : "Idle", true, 0.12f);
        const int rs = slot_of("Run"), is = slot_of("Idle");
        if (rs >= 0)
            kiln_skel_set_speed(&g_skel, (KilnSkelSlot)rs, clampf(speed / (GOBLIN_RUN_MPS * UNITS_PER_M), 0.6f, 3.6f));
        if (is >= 0) kiln_skel_set_speed(&g_skel, (KilnSkelSlot)is, 1.0f);
    }

    /* The head follows the Z-target, the way Link's does. */
    KilnActor *lock = kiln_actor_resolve(g_lock_h);
    const int look = lock && !air && st != KILN_PLAYER_ROLL;
    g_hero.look_w += ((look ? 1.0f : 0.0f) - g_hero.look_w) * 0.12f;
    if (g_hero.look_w > 0.01f && lock) {
        const float to = fm_atan2f(lock->xform.pos.v[0] - self->xform.pos.v[0],
                                   lock->xform.pos.v[2] - self->xform.pos.v[2]);
        const float a = clampf(wrap_pi(to - p->yaw), -1.1f, 1.1f) * g_hero.look_w;
        T3DQuat q;
        kiln_quat_axis_angle(&q, 0, 1, 0, a * 0.4f);
        kiln_skel_bone_rotate(&g_skel, g_b_neck, &q);
        kiln_quat_axis_angle(&q, 0, 1, 0, a * 0.6f);
        kiln_skel_bone_rotate(&g_skel, g_b_head, &q);
    }

    kiln_skel_update(&g_skel, dt);

    /* A crouch raises the feet (no root translation in the rig): lower him. */
    const T3DVec3 fl = kiln_skel_bone_pos(&g_skel, g_b_foot_l), fr = kiln_skel_bone_pos(&g_skel, g_b_foot_r);
    const float rise = (fl.v[1] < fr.v[1] ? fl.v[1] : fr.v[1]) - ANKLE_REST;
    const float want = !air && st != KILN_PLAYER_ROLL && rise > 0.0f ? -rise * GOBLIN_SCALE : 0.0f;
    g_hero.lift += (want - g_hero.lift) * 0.5f;
}

static void player_update(KilnActor *self, float dt)
{
    kiln_player_update(self, 1, dt);
    const KilnPlayer *p = kiln_player_of(self);
    if (KILN_OOT_PRIM_BODY) {
        /* The box hero's front is +Z: libdragon's axis-angle turns +Z to
         * (-sin t, cos t), so -yaw faces (sin yaw, cos yaw). */
        self->xform.rot_angle = -p->yaw;
        /* A roll reads as a tuck: the body squashes while kiln_player moves it. */
        const float sy = p->state == KILN_PLAYER_ROLL ? 0.6f : 1.0f;
        self->xform.scale = (fm_vec3_t){{ 1, sy, 1 }};
        return;
    }
    /* The goblin's nose is model -Z, so PI - yaw. */
    self->xform.rot_angle = PI - p->yaw;
    self->xform.scale = (fm_vec3_t){{ 1, 1, 1 }};
    hero_animate(self, p, dt);
}

static void prim_hero_draw(const KilnPlayer *p)
{
    kiln_prim_draw(&g_tunic);
    kiln_prim_draw(&g_head);
    kiln_prim_draw(&g_cap);
    kiln_prim_draw(&g_shield);

    /* The sword hangs back at rest and sweeps right-to-left across the front
     * over the attack. Local +Z is forward; the right hand is local -X. */
    float swing = -2.5f;
    if (p->state == KILN_PLAYER_ATTACK) {
        const float k = p->state_t / ATTACK_TIME;
        swing = -1.5f + 3.0f * (k > 1.0f ? 1.0f : k);
    }
    g_sword_xf.pos = (fm_vec3_t){{ -9, -2, 2 }};
    g_sword_xf.rot_angle = swing;
    kiln_transform_push(&g_sword_xf);
    kiln_prim_draw(&g_sword);
    kiln_transform_pop();
}

static void player_draw(KilnActor *self)
{
    const KilnPlayer *p = kiln_player_of(self);
    if (KILN_OOT_PRIM_BODY) {
        prim_hero_draw(p);
        return;
    }
    /* kiln_actor_draw_all has pushed the actor: its origin is the middle of
     * kiln_player's box, 16 above the feet. Two more levels: a pivot at the
     * tucked ball's centre that the roll turns about (+X turns +Y to -Z, his
     * forward), and the model, back down to its feet and scaled. */
    float roll = 0.0f;
    if (p->state == KILN_PLAYER_ROLL) {
        const float k = clampf(p->state_t / ROLL_TIME, 0.0f, 1.0f);
        roll = 2.0f * PI * k * k * (3.0f - 2.0f * k);
    }
    const float pivot = GOBLIN_ROLL_PIVOT_M * UNITS_PER_M;
    g_pivot_xf.pos = (fm_vec3_t){{ 0, KILN_PLAYER_MINS.v[1] + pivot + g_hero.lift, 0 }};
    g_pivot_xf.rot_angle = roll;
    g_body_xf.pos = (fm_vec3_t){{ 0, -pivot, 0 }};
    kiln_transform_push(&g_pivot_xf);
    kiln_transform_push(&g_body_xf);
    kiln_skel_draw(&g_skel);
    /* Model units from here: the bone matrices carry goblin.py's x64. hand_r's
     * +Y runs out along the fingers, so the blade does; hand_l's +X points out
     * of the left hand, so the buckler is thin along it. */
    kiln_skel_bone_push(&g_skel, g_b_hand_r);
    kiln_prim_draw(&g_guard);
    kiln_prim_draw(&g_blade);
    kiln_transform_pop();
    kiln_skel_bone_push(&g_skel, g_b_hand_l);
    kiln_prim_draw(&g_buckler);
    kiln_transform_pop();
    kiln_transform_pop();
    kiln_transform_pop();
}

static void player_event(KilnActor *self, uint16_t event_id,
                         const int32_t *args, uint8_t argc)
{
    (void)args;
    if (event_id == KILN_EV_PLAYER_FOOTSTEP && argc >= 1)
        kiln_sound_play("step_stone", self->xform.pos, 1.0f);
}

static const KilnActorProfile PROFILES[PROFILE_COUNT] = {
    [PROFILE_PLAYER] = { .name = "player", .category = KILN_ACTOR_CAT_PLAYER,
                          .state_size = KILN_PLAYER_STATE_SIZE,
                          .init = player_init, .update = player_update,
                          .draw = player_draw, .event = player_event },
    [PROFILE_ENEMY]  = { .name = "enemy", .category = KILN_ACTOR_CAT_ENEMY,
                          .state_size = sizeof(EnemyState),
                          .init = enemy_init, .update = enemy_update,
                          .draw = enemy_draw },
};

static KilnActor g_pool[ACTOR_POOL_CAP];
static KilnMap g_map;
static KilnCamera g_cam;
static KilnScene g_scene;
static KilnActorHandle g_player_h = KILN_ACTOR_HANDLE_NONE;
static KilnActorHandle g_lock_h   = KILN_ACTOR_HANDLE_NONE;

// ── Tapes ───────────────────────────────────────────────────────────────
static const KilnInputKey ATTRACT_KEYS[] = {
    { .frame =   0, .buttons = KILN_BTN_Z },
    { .frame =  10, .buttons = KILN_BTN_Z, .sy = 85 },
    { .frame =  45, .buttons = KILN_BTN_Z },
    { .frame =  60, .buttons = KILN_BTN_Z | KILN_BTN_A },
    { .frame =  66, .buttons = KILN_BTN_Z, .sy = 50 },
    { .frame =  90, .buttons = KILN_BTN_Z | KILN_BTN_A },
    { .frame =  96, .buttons = KILN_BTN_Z, .sx = 70 },
    { .frame = 140, .buttons = KILN_BTN_Z | KILN_BTN_A },
    { .frame = 146, .buttons = KILN_BTN_Z, .sy = 60 },
    { .frame = 176, .buttons = KILN_BTN_Z | KILN_BTN_A },
    { .frame = 182 },
    { .frame = 210, .sx = -85, .sy = 40 },
    { .frame = 290, .buttons = KILN_BTN_B, .sy = 85 },
    { .frame = 296, .sy = 85 },
    { .frame = 350, .buttons = KILN_BTN_L, .sx = 60 },
    { .frame = 356 },
    { .frame = 420 },
};
static const KilnInputTape ATTRACT = { ATTRACT_KEYS, 17, 0 };

static const KilnInputKey TARGET_KEYS[] = {
    { .frame =   0 },
    { .frame =  20, .buttons = KILN_BTN_Z },
    { .frame =  80, .buttons = KILN_BTN_Z | KILN_BTN_A },
    { .frame =  86, .buttons = KILN_BTN_Z },
};
static const KilnInputTape TARGET = { TARGET_KEYS, 4, KILN_INPUT_NO_LOOP };

/* Lock on, then keep swinging: the torso-masked Attack overlay on repeat. */
static const KilnInputKey ATTACK_KEYS[] = {
    { .frame =   0 },
    { .frame =  20, .buttons = KILN_BTN_Z },
    { .frame =  60, .buttons = KILN_BTN_Z | KILN_BTN_A },
    { .frame =  66, .buttons = KILN_BTN_Z },
    { .frame = 100, .buttons = KILN_BTN_Z },
};
static const KilnInputTape ATTACK_T = { ATTACK_KEYS, 5, 60 };

/* A tight circle at a run, dodge-rolling every 70 frames: a roll lasts 27, so
 * any moment is ~40% likely to be mid-spin. Tight so it stays off the walls,
 * and no jump, because kiln_player's jump clears the room's walls. */
static const KilnInputKey ROLL_KEYS[] = {
    { .frame =  0, .sx = 60, .sy = 85 },
    { .frame = 30, .buttons = KILN_BTN_L, .sx = 60, .sy = 85 },
    { .frame = 34, .sx = 60, .sy = 85 },
    { .frame = 70 },
};
static const KilnInputTape ROLL_T = { ROLL_KEYS, 4, 0 };

static fm_vec3_t cam_fwd(void)
{
    fm_vec3_t f = {{ g_scene.cam_target.v[0] - g_scene.cam_pos.v[0], 0,
                     g_scene.cam_target.v[2] - g_scene.cam_pos.v[2] }};
    fm_vec3_norm(&f, &f);
    return f;
}
static fm_vec3_t cam_right(void)
{
    const fm_vec3_t f = cam_fwd();
    return (fm_vec3_t){{ -f.v[2], 0, f.v[0] }};
}

/* kiln_clip ignores a brush a trace starts inside, so a spawn overlapping the
 * floor slab would fall straight through it. oot_test.map's start is at y 16
 * with a 16-unit half-height player — exactly inside the 0..4 floor. */
static fm_vec3_t lift_out(fm_vec3_t p)
{
    for (int pass = 0; pass < 4; pass++) {
        int moved = 0;
        for (int i = 0; i < g_world_count; i++) {
            const KilnBrush *b = &g_world[i];
            if (p.v[0] + KILN_PLAYER_MAXS.v[0] > b->mins.v[0] && p.v[0] + KILN_PLAYER_MINS.v[0] < b->maxs.v[0] &&
                p.v[1] + KILN_PLAYER_MAXS.v[1] > b->mins.v[1] && p.v[1] + KILN_PLAYER_MINS.v[1] < b->maxs.v[1] &&
                p.v[2] + KILN_PLAYER_MAXS.v[2] > b->mins.v[2] && p.v[2] + KILN_PLAYER_MINS.v[2] < b->maxs.v[2]) {
                p.v[1] = b->maxs.v[1] - KILN_PLAYER_MINS.v[1] + 0.5f;
                moved = 1;
            }
        }
        if (!moved) break;
    }
    return p;
}

static const char *state_name(int s)
{
    static const char *N[] = { "idle", "walk", "run", "roll", "attack", "jump", "fall" };
    return (s >= 0 && s < 7) ? N[s] : "?";
}

static void build_geometry(void)
{
    const fm_vec3_t O = {{ 0, 0, 0 }};
    kiln_prim_box(&g_tunic, (fm_vec3_t){{ 0, -5, 0 }}, (fm_vec3_t){{ 7, 11, 6 }},
                  kiln_prim_rgba(0x48, 0xB0, 0x50), kiln_prim_rgba(0x30, 0x88, 0x3C), kiln_prim_rgba(0x50, 0x38, 0x20));
    kiln_prim_box(&g_head, (fm_vec3_t){{ 0, 11, 0 }}, (fm_vec3_t){{ 5, 5, 5 }},
                  kiln_prim_rgba(0xF8, 0xD0, 0xA0), kiln_prim_rgba(0xE8, 0xB8, 0x88), kiln_prim_rgba(0xC0, 0x90, 0x60));
    kiln_prim_box(&g_cap, (fm_vec3_t){{ 0, 17, -3 }}, (fm_vec3_t){{ 4, 3, 6 }},
                  kiln_prim_rgba(0x40, 0xA0, 0x48), kiln_prim_rgba(0x2C, 0x80, 0x34), kiln_prim_rgba(0x20, 0x50, 0x24));
    kiln_prim_box(&g_shield, (fm_vec3_t){{ 9, -2, 2 }}, (fm_vec3_t){{ 1, 7, 5 }},
                  kiln_prim_rgba(0x60, 0x80, 0xE0), kiln_prim_rgba(0x40, 0x58, 0xC0), kiln_prim_rgba(0x30, 0x40, 0x80));
    kiln_prim_box(&g_sword, (fm_vec3_t){{ 0, 0, 12 }}, (fm_vec3_t){{ 1, 1, 12 }},
                  kiln_prim_rgba(0xF0, 0xF4, 0xFF), kiln_prim_rgba(0xB8, 0xC0, 0xD0), kiln_prim_rgba(0x80, 0x88, 0x98));
    kiln_prim_box(&g_enemy_body, O, (fm_vec3_t){{ 10, 9, 10 }},
                  kiln_prim_rgba(0xE8, 0x50, 0x60), kiln_prim_rgba(0xB8, 0x30, 0x48), kiln_prim_rgba(0x60, 0x18, 0x20));
    kiln_prim_box(&g_enemy_flash, O, (fm_vec3_t){{ 10, 9, 10 }},
                  0xFFFFFFFF, 0xFFF0F0FF, 0xE0D0D0FF);
    kiln_prim_box(&g_enemy_eye, O, (fm_vec3_t){{ 2, 3, 1 }},
                  0xFFFFFFFF, 0xFFFFFFFF, 0x202020FF);
    kiln_prim_floor(&g_shadow, 10.0f, 1, kiln_prim_rgba(0x14, 0x18, 0x14), kiln_prim_rgba(0x14, 0x18, 0x14));
    kiln_prim_box(&g_pillar, (fm_vec3_t){{ 0, 24, 0 }}, (fm_vec3_t){{ 7, 24, 7 }},
                  kiln_prim_rgba(0xC8, 0xC0, 0xA8), kiln_prim_rgba(0x80, 0x7C, 0x70), kiln_prim_rgba(0x40, 0x40, 0x3C));
    kiln_prim_box(&g_flame, (fm_vec3_t){{ 0, 52, 0 }}, (fm_vec3_t){{ 4, 4, 4 }},
                  kiln_prim_rgba(0xFF, 0xF0, 0x80), kiln_prim_rgba(0xFF, 0x98, 0x20), kiln_prim_rgba(0xFF, 0x60, 0x10));

    kiln_transform_init(&g_sword_xf);
    g_sword_xf.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
    for (int i = 0; i < 2; i++) {
        kiln_transform_init(&g_eye_xf[i]);
        g_eye_xf[i].pos = (fm_vec3_t){{ i ? 4.0f : -4.0f, 3, 10 }};
    }
    kiln_transform_init(&g_shadow_xf);
    kiln_transform_init(&g_deco_xf);

    /* The goblin's props, in MODEL units (bone matrices carry the x64). */
    kiln_prim_box(&g_blade, (fm_vec3_t){{ 0, 34, 0 }}, (fm_vec3_t){{ 1.4f, 24, 3.2f }},
                  kiln_prim_rgba(0xF4, 0xF6, 0xFF), kiln_prim_rgba(0xB8, 0xC4, 0xD8), kiln_prim_rgba(0x70, 0x78, 0x88));
    kiln_prim_box(&g_guard, (fm_vec3_t){{ 0, 9, 0 }}, (fm_vec3_t){{ 2.0f, 1.6f, 8.0f }},
                  kiln_prim_rgba(0xF0, 0xC0, 0x40), kiln_prim_rgba(0xB0, 0x80, 0x20), kiln_prim_rgba(0x60, 0x40, 0x10));
    kiln_prim_box(&g_buckler, (fm_vec3_t){{ 4, 2, 0 }}, (fm_vec3_t){{ 1.4f, 12, 11 }},
                  kiln_prim_rgba(0x60, 0x80, 0xE0), kiln_prim_rgba(0x40, 0x58, 0xC0), kiln_prim_rgba(0x30, 0x40, 0x80));
    kiln_transform_init(&g_pivot_xf);
    g_pivot_xf.rot_axis = (fm_vec3_t){{ 1, 0, 0 }};
    kiln_transform_init(&g_body_xf);
    g_body_xf.scale = (fm_vec3_t){{ GOBLIN_SCALE, GOBLIN_SCALE, GOBLIN_SCALE }};
}

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);
    kiln_input_init();
    kiln_audio_init(KILN_AUDIO_DEFAULT);

    int sfx_step = kiln_sfx_load("rom:/sfx/step.wav64");
    kiln_surface_register(0, &(KilnSurfaceDef){ .friction = 0.9f, .footstep_sfx = sfx_step });
    KilnSoundShader shaders[] = {
        { .name = "step_stone", .wav64_path = "rom:/sfx/step.wav64", .base_vol = 0.6f, .falloff_radius = 0.0f },
        { .name = "hit",        .wav64_path = "rom:/sfx/impact.wav64", .base_vol = 0.9f, .falloff_radius = 0.0f },
    };
    kiln_sound_init(shaders, 2);

    kiln_actor_system_init(PROFILES, PROFILE_COUNT, g_pool, ACTOR_POOL_CAP);
    kiln_event_init();
    build_geometry();

    if (!KILN_OOT_PRIM_BODY) {
#if !KILN_OOT_PRIM_BODY
        /* mkBlenderModel compresses the .t3dm (mkasset -c 2). Preprocessed out
         * rather than faked on the host: plat/host has no decompressor, and a
         * pc-* build ships no skinned model to decompress. */
        asset_init_compression(2);
#endif
        g_goblin = t3d_model_load("rom:/models/goblin.t3dm");
        kiln_skel_create(&g_skel, g_goblin);
        kiln_skel_play(&g_skel, "Idle", true);
        /* By NAME: the exporter's bone order is not goblin.py's. */
        g_b_hand_r = kiln_skel_bone(&g_skel, "hand_r");
        g_b_hand_l = kiln_skel_bone(&g_skel, "hand_l");
        g_b_foot_l = kiln_skel_bone(&g_skel, "foot_l");
        g_b_foot_r = kiln_skel_bone(&g_skel, "foot_r");
        g_b_neck = kiln_skel_bone(&g_skel, "neck");
        g_b_head = kiln_skel_bone(&g_skel, "head");
        g_upper = kiln_skel_mask_bone(&g_skel, "torso");
    }

#ifdef KILN_DEBUG
    kiln_prof_init();
    kiln_console_init();
    kiln_console_log("oot-demo debug console ready");
    kiln_console_log("hold Start + C-Up Left Down Right");
#endif

    kiln_map_register_classname("info_player_start", PROFILE_PLAYER);
    kiln_map_register_classname("info_enemy", PROFILE_ENEMY);

    const int loaded = kiln_map_load(&g_map, "rom:/maps/oot_test.map") == 0;
    if (!loaded) debugf("oot-demo: kiln_map_load failed\n");
    kiln_map_tint(&g_map, &(KilnMapTint){
        .floor = kiln_prim_rgba(0x6C, 0x88, 0x5C), .floor_edge = kiln_prim_rgba(0x40, 0x54, 0x3C),
        .floor_y = 4.5f, .floor_radius = 141.0f,
        .top = kiln_prim_rgba(0xC8, 0xBC, 0x9C),
        .wall_low = kiln_prim_rgba(0x3C, 0x42, 0x3A), .wall_high = kiln_prim_rgba(0xA4, 0xAC, 0x98),
        .z_face_shade = 0.86f,
        .underside = kiln_prim_rgba(0x28, 0x28, 0x24),
    });

    for (int i = 0; i < g_map.brush_count && g_world_count < 30; i++) g_world[g_world_count++] = g_map.brushes[i];
    for (int i = 0; i < 2; i++) {
        const fm_vec3_t c = PILLAR_AT[i];
        g_world[g_world_count++] = (KilnBrush){ .mins = {{ c.v[0] - 7, 0, c.v[2] - 7 }},
                                                .maxs = {{ c.v[0] + 7, 48, c.v[2] + 7 }} };
    }
    kiln_clip_set_world(g_world, g_world_count);

    fm_vec3_t ppos = {{ 0, 16, 0 }};
    float pyaw = 0.0f;
    int spawned_player = 0, spawned_enemies = 0;
    for (int i = 0; i < g_map.spawn_count; i++) {
        KilnRoomSpawn *s = &g_map.spawns[i];
        if (s->profile_id == PROFILE_PLAYER && !spawned_player) {
            ppos = s->pos; pyaw = s->yaw;
            spawned_player = 1;
        } else if (s->profile_id == PROFILE_ENEMY && spawned_enemies < 2) {
            kiln_actor_spawn(s->profile_id, s->pos, s->yaw, &s->dict);
            spawned_enemies++;
        }
    }
    /* Beside the +X creature, with the torch pillar at (64,-64) off the line
     * from the targeting camera to the player. */
    if (KILN_JUMP == JUMP_TARGET || KILN_JUMP == JUMP_ATTACK) ppos = (fm_vec3_t){{ 40, 16, 10 }};
    ppos = lift_out(ppos);
    g_player_h = kiln_actor_spawn(PROFILE_PLAYER, ppos, pyaw, NULL);

    kiln_scene_init(&g_scene);
    kiln_prim_stage(&g_scene, RGBA32(0x2A, 0x34, 0x48, 0xFF), 240.0f, 560.0f);
    g_scene.fov_deg = 62.0f;
    g_scene.near_z = 10.0f;
    g_scene.far_z = 560.0f;

    /* Boom in world units. kiln_camera_init's defaults (distance 6, height 3)
     * are sized for a unit-scale scene; against a 32-unit player they put the
     * eye inside the player, which this demo shipped with. */
    kiln_camera_init(&g_cam);
    g_cam.distance = 96.0f;
    g_cam.height = 74.0f;      /* high enough that, locked on, the creature */
    g_cam.look_height = 4.0f;  /* shows over the player's head, not behind it */
    kiln_camera_set_collision(&g_cam, 1);
    kiln_camera_snap(&g_cam, ppos, pyaw);

    float cutscene_t = 0.0f;
    const float CUTSCENE_LEN = 2.6f;
    if (KILN_JUMP == JUMP_TARGET) {
        kiln_input_play(1, &TARGET);
    } else if (KILN_JUMP == JUMP_ATTACK) {
        kiln_input_play(1, &ATTACK_T);
    } else if (KILN_JUMP == JUMP_ROLL) {
        kiln_input_play(1, &ROLL_T);
    } else {
        kiln_camera_push(&g_cam, KILN_CAM_CUTSCENE);
        cutscene_t = CUTSCENE_LEN;
        kiln_input_set_attract(1, &ATTRACT, 180);   /* after the 2.6 s orbit */
    }

    int z_held = 0, defeated = 0, hits = 0;
    uint32_t swing_id = 0, last_swing_hit = 0;
    uint32_t frames = 0;
    float fps = 60.0f;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        const float dt = 1.0f / 60.0f;
#ifdef KILN_DEBUG
        KILN_PROF_BEGIN(KILN_PROF_UPDATE);
#endif
        kiln_input_update();
#ifdef KILN_DEBUG
        kiln_console_update(1);
#endif
        const KilnInput *in = kiln_input_get(1);

        kiln_player_set_camera_basis(cam_fwd(), cam_right());
        kiln_event_process(dt);
        kiln_actor_update_all(dt);

        KilnActor *player = kiln_actor_resolve(g_player_h);
        const KilnPlayer *pl = player ? kiln_player_of(player) : NULL;
        const fm_vec3_t pnow = player ? player->xform.pos : ppos;

        /* ── Sword hits: the middle of the swing, once per swing ───────── */
        if (pl && pl->state == KILN_PLAYER_ATTACK) {
            if (pl->state_t < dt * 1.5f) swing_id++;
            const float k = pl->state_t / ATTACK_TIME;
            if (k > 0.2f && k < 0.8f && last_swing_hit != swing_id) {
                const fm_vec3_t f = {{ fm_sinf(pl->yaw), 0, fm_cosf(pl->yaw) }};
                for (KilnActor *e = kiln_actor_first(KILN_ACTOR_CAT_ENEMY); e; e = kiln_actor_next(e)) {
                    EnemyState *s = (EnemyState *)e->state;
                    if (s->dead_t > 0.0f) continue;
                    const float dx = e->xform.pos.v[0] - pnow.v[0], dz = e->xform.pos.v[2] - pnow.v[2];
                    const float d2 = dx * dx + dz * dz;
                    if (d2 > 38.0f * 38.0f) continue;
                    /* In front, within ~78 degrees: dot/|d| > 0.2, squared to
                     * keep libm out of it. */
                    const float dot = dx * f.v[0] + dz * f.v[2];
                    if (d2 > 1.0f && (dot < 0.0f || dot * dot < 0.04f * d2)) continue;
                    s->hp--; s->flash = 0.25f; s->hop_v = 150.0f; hits++;
                    last_swing_hit = swing_id;
                    kiln_sound_play("hit", e->xform.pos, 1.0f);
                    if (s->hp <= 0) {
                        s->dead_t = 3.0f;
                        defeated++;
                        if (kiln_actor_handle_of(e) == g_lock_h && g_cam.mode == KILN_CAM_TARGETING) {
                            kiln_camera_pop(&g_cam);
                            g_lock_h = KILN_ACTOR_HANDLE_NONE;
                        }
                    }
                }
            }
        }

        /* ── Z-targeting: lock on the rising edge, release on let-go ───── */
        const int z_now = (in->buttons & KILN_BTN_Z) != 0;
        const int z_edge = z_now && !z_held;
        z_held = z_now;
        /* Lock on the press — or, if Z is still held with nothing locked
         * (it was pressed during the opening orbit, or nothing was in the cone
         * yet), keep trying until something is. */
        if (g_cam.mode == KILN_CAM_NORMAL && (z_edge || (z_now && g_lock_h == KILN_ACTOR_HANDLE_NONE))) {
            KilnActorHandle h = kiln_target_acquire(g_scene.cam_pos, cam_fwd(), 0.8f, 240.0f);
            KilnActor *t = kiln_actor_resolve(h);
            if (t && ((EnemyState *)t->state)->dead_t <= 0.0f) {
                kiln_camera_push(&g_cam, KILN_CAM_TARGETING);
                kiln_camera_set_target_actor(&g_cam, h);
                g_lock_h = h;
            }
        } else if (g_cam.mode == KILN_CAM_TARGETING && !z_now) {
            kiln_camera_pop(&g_cam);
            g_lock_h = KILN_ACTOR_HANDLE_NONE;
        }

        /* ── Opening orbit: half a turn around the player, then NORMAL ─── */
        if (g_cam.mode == KILN_CAM_CUTSCENE) {
            cutscene_t -= dt;
            const float a = 3.1416f * (1.0f - cutscene_t / CUTSCENE_LEN) + 0.4f;
            kiln_camera_set_cutscene(&g_cam,
                (fm_vec3_t){{ pnow.v[0] + fm_sinf(a) * 130.0f, pnow.v[1] + 70.0f - 30.0f * (1.0f - cutscene_t / CUTSCENE_LEN),
                              pnow.v[2] + fm_cosf(a) * 130.0f }},
                (fm_vec3_t){{ pnow.v[0], pnow.v[1] + 8, pnow.v[2] }});
            if (cutscene_t <= 0.0f) kiln_camera_pop(&g_cam);
        }

        kiln_camera_update(&g_cam, pnow, pl ? pl->yaw : pyaw, dt);
        kiln_camera_apply(&g_cam, &g_scene);
        kiln_scene_update(&g_scene);
        kiln_sound_update_listener(g_scene.cam_pos, cam_fwd());

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }
#ifdef KILN_DEBUG
        KILN_PROF_END(KILN_PROF_UPDATE);
        kiln_prof_frame_done();
#endif

        /* ── 3D ───────────────────────────────────────────────────── */
        kiln_frame_begin();
#ifdef KILN_DEBUG
        KILN_PROF_BEGIN(KILN_PROF_SCENE);
#endif
        kiln_scene_begin(&g_scene);
        kiln_map_draw(&g_map);
        for (int i = 0; i < 2; i++) {
            g_deco_xf.pos = PILLAR_AT[i];
            g_deco_xf.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
            g_deco_xf.rot_angle = (float)frames * 0.08f + i;   /* the flame turns */
            kiln_transform_push(&g_deco_xf);
            kiln_prim_draw(&g_pillar);
            kiln_prim_draw(&g_flame);
            kiln_transform_pop();
        }
        for (KilnActor *a = kiln_actor_first(KILN_ACTOR_CAT_ENEMY); a; a = kiln_actor_next(a)) {
            if (((EnemyState *)a->state)->dead_t > 0.0f) continue;
            g_shadow_xf.pos = (fm_vec3_t){{ a->xform.pos.v[0], 4.4f, a->xform.pos.v[2] }};
            kiln_transform_push(&g_shadow_xf);
            kiln_prim_draw(&g_shadow);
            kiln_transform_pop();
        }
        g_shadow_xf.pos = (fm_vec3_t){{ pnow.v[0], 4.4f, pnow.v[2] }};
        kiln_transform_push(&g_shadow_xf);
        kiln_prim_draw(&g_shadow);
        kiln_transform_pop();
        kiln_actor_draw_all();
#ifdef KILN_DEBUG
        KILN_PROF_END(KILN_PROF_SCENE);
#endif

        /* ── 2D ───────────────────────────────────────────────────── */
        kiln_gui_begin();
#ifdef KILN_DEBUG
        KILN_PROF_BEGIN(KILN_PROF_GUI);
#endif
        const color_t ink = RGBA32(0xE8, 0xE8, 0xF0, 0xFF);
        const color_t teal = RGBA32(0x00, 0xF5, 0xD4, 0xFF);
        const char *mode_s = g_cam.mode == KILN_CAM_TARGETING ? "target"
                           : g_cam.mode == KILN_CAM_CUTSCENE ? "cutscene" : "follow";
        kiln_gui_panel(8, 8, 150, 78, RGBA32(0x0C, 0x10, 0x1C, 0xFF), teal);
        kiln_gui_text(14, 21, teal, "KILN OOT");
        kiln_gui_text(14, 34, ink, "cam %-8s %s", mode_s, pl ? state_name(pl->state) : "-");
        kiln_gui_text(14, 46, ink, "hits %d  defeated %d", hits, defeated);
        kiln_gui_text(14, 58, ink, "pos %4.0f %4.0f", pnow.v[0], pnow.v[2]);
        if (KILN_OOT_PRIM_BODY) {
            kiln_gui_text(14, 70, RGBA32(0x90, 0x98, 0xB0, 0xFF), "box hero (host)");
        } else {
            const int heavy = g_skel.blend_factor >= 0.5f ? KILN_SKEL_BLEND : KILN_SKEL_BASE;
            const char *clip = kiln_skel_clip(&g_skel, (KilnSkelSlot)heavy);
            const char *over = kiln_skel_clip(&g_skel, KILN_SKEL_OVERLAY);
            kiln_gui_text(14, 70, RGBA32(0xFF, 0xC8, 0x60, 0xFF), "anim %s x%.1f%s%s", clip ? clip : "-",
                          (double)g_skel.slot_speed[heavy], over ? " +" : "", over ? over : "");
        }
        kiln_gui_text(14, 82, RGBA32(0x90, 0x98, 0xB0, 0xFF), "%4.1f fps", fps);
        if (!loaded) kiln_gui_text(14, 100, RGBA32(0xFF, 0x50, 0x50, 0xFF), "MAP DID NOT LOAD");

        if (g_lock_h != KILN_ACTOR_HANDLE_NONE) {
            KilnActor *t = kiln_actor_resolve(g_lock_h);
            if (t) {
                kiln_dd_begin(&g_scene, SCREEN_W, SCREEN_H);
                kiln_dd_line((fm_vec3_t){{ pnow.v[0], pnow.v[1] + 6, pnow.v[2] }}, t->xform.pos,
                             RGBA32(0xFF, 0x60, 0xA0, 0xFF));
                kiln_dd_end();
                kiln_target_draw_reticle(&g_scene, t->xform.pos, SCREEN_W, SCREEN_H,
                                         RGBA32(0xFF, 0xE0, 0x40, 0xFF));
            }
        }

        /* Bottom-right, above the hint bar: in oot-demo-debug the console's log
         * panel occupies the top-right corner and covered it. */
        if (kiln_input_scripted(1)) {
            kiln_gui_panel(SCREEN_W - 58, SCREEN_H - 44, 50, 16, RGBA32(0xC0, 0x30, 0x60, 0xFF),
                           RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
            kiln_gui_text(SCREEN_W - 49, SCREEN_H - 32, RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "DEMO");
        }
        kiln_gui_panel(8, SCREEN_H - 24, SCREEN_W - 16, 16,
                       RGBA32(0x0C, 0x10, 0x1C, 0xFF), RGBA32(0x8B, 0x5C, 0xF6, 0xFF));
        kiln_gui_text(14, SCREEN_H - 12, ink, "Z target  A sword  B jump  L roll  R run");

#ifdef KILN_DEBUG
        kiln_console_draw();
        KILN_PROF_END(KILN_PROF_GUI);
#endif
        kiln_gui_end();
        kiln_frame_end();

        kiln_sound_update();
        kiln_audio_update();
    }
}
