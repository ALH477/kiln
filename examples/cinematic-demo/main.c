// SPDX-License-Identifier: MIT
//
// Cinematic demo — a 60-second looping scene in one hangar. The Interceptor
// sits on its pad while two service droids work their way round it, waving at
// the hull. The goblin captain walks the perimeter and shoulders past a stack
// of crates, which topples. At 22 s the bay door swings open; two aliens walk
// out of the corridor behind it to a stand-off with the captain, the lead one
// gets a target reticle, and at 44 s they back off through the door, which
// closes behind them before the loop comes round.
//
// Where everyone is at time t lives in cine_script.h as pure functions of t,
// with the cast's measured sizes. This file draws them.
//
//   kiln_engine    frame / scene / transforms, fog + two lights
//   kiln_map       assets/hangar.map: brushes, spawns (kiln_dict epairs), tint
//   kiln_clip      the map's brushes are the world the crates fall in
//   kiln_physics   crate stacks; the goblin and an alien knock one each over
//   kiln_prim      crates and the door as 24-vertex flat-shaded boxes
//   kiln_skel      goblin Idle/Walk, droid Idle/Wave, alien Idle/Approach —
//                  one skeleton per actor, blended by how fast it moves
//   kiln_actor     profiles, category draw order
//   kiln_event     the door's 500 ms-delayed OPEN and CLOSE
//   kiln_target    the reticle on the lead alien, 30-36 s
//   kiln_camera    CUTSCENE mode, fed by cine_shots.h: seven kiln_camkey
//                  shots cut together, kiln_camlint-checked in the flake
//   kiln_gui       letterbox, caption cards, timecode
//   kiln_audio     music bed; kiln_sound positional footsteps and thumps

#include <libdragon.h>
#include <t3d/t3dmodel.h>

#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_actor.h>
#include <kiln/kiln_event.h>
#include <kiln/kiln_audio.h>
#include <kiln/kiln_sound.h>
#include <kiln/kiln_dict.h>
#include <kiln/kiln_map.h>
#include <kiln/kiln_clip.h>
#include <kiln/kiln_physics.h>
#include <kiln/kiln_prim.h>
#include <kiln/kiln_target.h>
#include <kiln/kiln_camera.h>
#include <kiln/kiln_skel.h>
#include <kiln/kiln_debugdraw.h>

#include <string.h>

#include "cine_script.h"
#include "cine_shots.h"

// Jump ROMs (nix/demos/cinematic-demo.nix). T10/T22/T30/T38 fast-forward the
// timeline to that second at boot and HOLD it there, since Ares' boot time
// varies by a second or two. BOXES runs the loop with every actor's measured
// bounds drawn over it, which shows a model at the wrong size, or standing in
// the floor, in one capture.
enum { JUMP_NONE, JUMP_T10, JUMP_T22, JUMP_T30, JUMP_T38, JUMP_BOXES };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

static float jump_time(void)
{
    switch (KILN_JUMP) {
    case JUMP_T10: return 10.0f;
    case JUMP_T22: return 22.0f;
    case JUMP_T30: return 30.0f;
    case JUMP_T38: return 38.0f;
    default:       return -1.0f;
    }
}

#define SCREEN_W        320
#define SCREEN_H        240
#define DT              (1.0f / 60.0f)

enum { EV_DOOR_OPEN = 1, EV_DOOR_CLOSE = 2 };

enum {
    PROFILE_GOBLIN,
    PROFILE_DROID,
    PROFILE_ALIEN,
    PROFILE_DOOR,
    PROFILE_COUNT,
};

#define ACTOR_POOL_CAP 16
static KilnActor g_pool[ACTOR_POOL_CAP];

// ── Assets ──────────────────────────────────────────────────────────────
static T3DModel *g_ship_model, *g_goblin_model, *g_droid_model, *g_alien_model;

// One skeleton per animated actor. Two droids sharing one skeleton would wave
// in lockstep and could never be parked and rolling at the same time; a
// skinned model drawn with plain t3d_model_draw has no bone matrices at all.
#define DROID_MAX 2
#define ALIEN_MAX 2
static KilnSkel g_goblin_skel;
static KilnSkel g_droid_skel[DROID_MAX];
static KilnSkel g_alien_skel[ALIEN_MAX];

static int g_music = -1, g_music_ch = -1;

static KilnMap g_map;
static KilnScene g_scene;
static KilnCamera g_cam;
static KilnTransform g_ship_xf;

static KilnPrim g_door_prim;
static KilnPrim g_crate_prim;

// ── Crates ──────────────────────────────────────────────────────────────
// kiln_physics bodies never rotate (its header says why). A toppled crate that
// slides off a stack bolt upright reads as a lift, not a fall, so each crate
// carries a DRAWN tumble: it rolls about the axis across its velocity while
// airborne and settles to the nearest quarter-turn once it lands — where a
// cube looks like a cube again, now lying on a different face.
#define CRATE_MAX 5
static KilnPhysicsWorld g_pworld;
static KilnPhysicsBody g_bodies[CRATE_MAX];
static KilnTransform g_crate_xf[CRATE_MAX];
static float g_crate_ang[CRATE_MAX];
static fm_vec3_t g_crate_axis[CRATE_MAX];
static uint8_t g_crate_ground[CRATE_MAX];
static float g_crate_air[CRATE_MAX];

static const fm_vec3_t CRATE_START[CRATE_MAX] = {
    {{ CINE_STACK_A_X, CINE_FLOOR_Y + CINE_CRATE_HALF + 0.1f,  CINE_STACK_A_Z }},
    {{ CINE_STACK_A_X, CINE_FLOOR_Y + CINE_CRATE_HALF * 3.0f + 0.3f, CINE_STACK_A_Z }},
    {{ CINE_STACK_A_X, CINE_FLOOR_Y + CINE_CRATE_HALF * 5.0f + 0.5f, CINE_STACK_A_Z }},
    {{ CINE_STACK_B_X, CINE_FLOOR_Y + CINE_CRATE_HALF + 0.1f,  CINE_STACK_B_Z }},
    {{ CINE_STACK_B_X, CINE_FLOOR_Y + CINE_CRATE_HALF * 3.0f + 0.3f, CINE_STACK_B_Z }},
};

// ── Time ────────────────────────────────────────────────────────────────
static float g_t = 0.0f;           // 0..CINE_LOOP_T
static int   g_audible = 1;        // 0 while fast-forwarding (no SFX)

static KilnActorHandle g_goblin_h = KILN_ACTOR_HANDLE_NONE;
static KilnActorHandle g_alien_h[ALIEN_MAX] = { KILN_ACTOR_HANDLE_NONE, KILN_ACTOR_HANDLE_NONE };
static KilnActorHandle g_door_h = KILN_ACTOR_HANDLE_NONE;

static int crossed(float t_prev, float t_now, float at)
{
    return t_prev < at && t_now >= at;
}

// kiln_actor_draw_all pushes each actor's xform before calling its draw, so
// the scale and yaw go INTO the xform here and draw callbacks push nothing.
// Pushing it again inside a callback applies the matrix twice: scale squared
// (the cast came out a tenth of their size) and every position displaced by
// its own scaled copy — which is exactly how this demo first rendered.
static void apply_pose(KilnActor *self, const CinePose *p)
{
    self->xform.pos = p->pos;
    self->xform.scale = (fm_vec3_t){{ CINE_SCALE, CINE_SCALE, CINE_SCALE }};
    self->xform.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
    self->xform.rot_angle = p->yaw;
}

// ── Goblin ──────────────────────────────────────────────────────────────
typedef struct { float last_step; } GoblinState;

static void goblin_init(KilnActor *self, const KilnDict *args)
{
    (void)args;
    ((GoblinState *)self->state)->last_step = 0.0f;
}

static void goblin_update(KilnActor *self, float dt)
{
    (void)dt;
    const CinePose p = cine_goblin(g_t);
    apply_pose(self, &p);
    kiln_skel_set_blend(&g_goblin_skel, p.walk);

    // A footstep every third of a second of walking: the Walk clip is 40
    // frames, two steps. Keyed on walked time, so a stopped goblin is silent.
    GoblinState *s = (GoblinState *)self->state;
    const float w = cine_goblin_walked(g_t);
    if (w < s->last_step) s->last_step = w;               // loop wrapped
    if (w - s->last_step >= 0.333f) {
        s->last_step = w;
        if (g_audible) kiln_sound_play("step_stone", p.pos, 1.0f);
    }
}

static void goblin_draw(KilnActor *self) { (void)self; kiln_skel_draw(&g_goblin_skel); }

// ── Droids ──────────────────────────────────────────────────────────────
typedef struct { int idx; float radius, phase; } DroidState;

static void droid_init(KilnActor *self, const KilnDict *args)
{
    static int next;
    DroidState *s = (DroidState *)self->state;
    s->idx = next++ % DROID_MAX;
    s->radius = (float)kiln_dict_get_int(args, "radius", 26);
    s->phase = (float)kiln_dict_get_int(args, "phase", 0) * (CINE_PI / 180.0f);
}

static void droid_update(KilnActor *self, float dt)
{
    (void)dt;
    DroidState *s = (DroidState *)self->state;
    const CinePose p = cine_droid(s->radius, s->phase, g_t);
    apply_pose(self, &p);
    // Primary Wave, blended toward Idle's hover while rolling between stations.
    kiln_skel_set_blend(&g_droid_skel[s->idx], p.walk);
}

static void droid_draw(KilnActor *self)
{
    kiln_skel_draw(&g_droid_skel[((DroidState *)self->state)->idx]);
}

// ── Aliens ──────────────────────────────────────────────────────────────
typedef struct { int idx; float last_step; } AlienState;

static void alien_init(KilnActor *self, const KilnDict *args)
{
    AlienState *s = (AlienState *)self->state;
    const char *path = kiln_dict_get_str(args, "path", "approach");
    s->idx = (strcmp(path, "approach_late") == 0) ? 1 : 0;
    s->last_step = 0.0f;
}

static void alien_update(KilnActor *self, float dt)
{
    AlienState *s = (AlienState *)self->state;
    const CinePose g = cine_goblin(g_t);
    const CinePose p = cine_alien(s->idx, g_t, g.pos);
    apply_pose(self, &p);
    kiln_skel_set_blend(&g_alien_skel[s->idx], p.walk);

    s->last_step += dt * p.walk;
    if (s->last_step >= 0.5f) {                      // Approach is 30 frames
        s->last_step -= 0.5f;
        if (g_audible) kiln_sound_play("step_alien", p.pos, 1.0f);
    }
}

static void alien_draw(KilnActor *self)
{
    kiln_skel_draw(&g_alien_skel[((AlienState *)self->state)->idx]);
}

// ── Door ────────────────────────────────────────────────────────────────
// A slab hinged on the doorway's -X edge (kiln_prim_box's offset), swinging
// into the corridor. Driven by events so kiln_event's delay is what times it.
typedef struct { float cur, target; } DoorState;

static void door_init(KilnActor *self, const KilnDict *args)
{
    (void)args;
    DoorState *s = (DoorState *)self->state;
    s->cur = s->target = 0.0f;
}

static void door_event(KilnActor *self, uint16_t id, const int32_t *args, uint8_t argc)
{
    (void)args; (void)argc;
    DoorState *s = (DoorState *)self->state;
    if (id == EV_DOOR_OPEN)  s->target = 0.5f * CINE_PI * 0.94f;
    if (id == EV_DOOR_CLOSE) s->target = 0.0f;
    if (g_audible) kiln_sound_play("thump", self->xform.pos, 0.7f);
}

static void door_update(KilnActor *self, float dt)
{
    DoorState *s = (DoorState *)self->state;
    float k = 3.0f * dt;
    if (k > 1.0f) k = 1.0f;
    s->cur += (s->target - s->cur) * k;
    self->xform.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
    self->xform.rot_angle = s->cur;
}

static void door_draw(KilnActor *self) { (void)self; kiln_prim_draw(&g_door_prim); }

static const KilnActorProfile PROFILES[PROFILE_COUNT] = {
    [PROFILE_GOBLIN] = { .name = "goblin", .category = KILN_ACTOR_CAT_PLAYER,
        .state_size = sizeof(GoblinState),
        .init = goblin_init, .update = goblin_update, .draw = goblin_draw },
    [PROFILE_DROID] = { .name = "droid", .category = KILN_ACTOR_CAT_NPC,
        .state_size = sizeof(DroidState),
        .init = droid_init, .update = droid_update, .draw = droid_draw },
    [PROFILE_ALIEN] = { .name = "alien", .category = KILN_ACTOR_CAT_ENEMY,
        .state_size = sizeof(AlienState),
        .init = alien_init, .update = alien_update, .draw = alien_draw },
    [PROFILE_DOOR] = { .name = "door", .category = KILN_ACTOR_CAT_DOOR,
        .state_size = sizeof(DoorState),
        .init = door_init, .update = door_update, .event = door_event, .draw = door_draw },
};

// ── Crates ──────────────────────────────────────────────────────────────
static void crates_reset(void)
{
    kiln_physics_init(&g_pworld, g_bodies, CRATE_MAX);
    g_pworld.gravity = -90.0f;     // ~14 m/s^2 at 6.4 units/m: a touch brisk
    const fm_vec3_t half = {{ CINE_CRATE_HALF, CINE_CRATE_HALF, CINE_CRATE_HALF }};
    for (int i = 0; i < CRATE_MAX; i++) {
        kiln_physics_spawn(&g_pworld, KILN_PHYS_DYNAMIC, CRATE_START[i], half, 1.0f);
        g_crate_ang[i] = 0.0f;
        g_crate_axis[i] = (fm_vec3_t){{ 1, 0, 0 }};
        g_crate_ground[i] = 1;
        g_crate_air[i] = 0.0f;
    }
}

static void crates_update(float dt)
{
    for (int i = 0; i < g_pworld.count; i++) {
        KilnPhysicsBody *b = &g_bodies[i];
        const float vx = b->vel.v[0], vz = b->vel.v[2];
        const float h2 = vx * vx + vz * vz;
        // Airborne time bounds the tumble. A crate that ends up wedged
        // against a wall or another crate, never reporting on_ground, still
        // settles to a quarter-turn and does not stay a 45-degree plank.
        g_crate_air[i] = b->on_ground ? 0.0f : g_crate_air[i] + dt;
        if (!b->on_ground && h2 > 25.0f && g_crate_air[i] < 0.6f) {
            // Tip over toward the direction of travel: axis = up x velocity.
            g_crate_axis[i] = (fm_vec3_t){{ vz, 0.0f, -vx }};
            fm_vec3_norm(&g_crate_axis[i], &g_crate_axis[i]);
            g_crate_ang[i] += 5.0f * dt;
        } else {
            const float q = 0.5f * CINE_PI;
            const float snap = (float)(int)(g_crate_ang[i] / q + 0.5f) * q;
            g_crate_ang[i] += (snap - g_crate_ang[i]) * cine_clamp01(12.0f * dt);
        }
        if (b->on_ground && !g_crate_ground[i] && g_audible)
            kiln_sound_play("thump", b->pos, 0.5f);
        g_crate_ground[i] = b->on_ground;
    }
}

static void crates_draw(void)
{
    for (int i = 0; i < g_pworld.count; i++) {
        KilnTransform *xf = &g_crate_xf[i];
        xf->pos = g_bodies[i].pos;
        xf->rot_axis = g_crate_axis[i];
        xf->rot_angle = g_crate_ang[i];
        kiln_transform_push(xf);
        kiln_prim_draw(&g_crate_prim);
        kiln_transform_pop();
    }
}

// ── One step of the timeline, without drawing ──────────────────────────
static void sim_step(float dt)
{
    const float t0 = g_t;
    float t1 = g_t + dt;
    int wrapped = 0;
    if (t1 >= CINE_LOOP_T) { t1 -= CINE_LOOP_T; wrapped = 1; }

    if (crossed(t0, t1, CINE_DOOR_OPEN_T))
        kiln_event_post(g_door_h, EV_DOOR_OPEN, 500, NULL, 0, 1);
    if (crossed(t0, t1, CINE_DOOR_CLOSE_T))
        kiln_event_post(g_door_h, EV_DOOR_CLOSE, 500, NULL, 0, 1);

    // The goblin shoulders the top of stack A along his walk; the late
    // alien's leg catches stack B on its way out.
    if (crossed(t0, t1, CINE_BUMP_A_T)) {
        const float th = CINE_GOB_THETA0 + CINE_GOB_OMEGA * cine_goblin_walked(CINE_BUMP_A_T);
        const float tx = -fm_sinf(th), tz = fm_cosf(th);
        kiln_physics_apply_impulse(&g_bodies[2], (fm_vec3_t){{ tx * 42.0f, 30.0f, tz * 42.0f }});
        kiln_physics_apply_impulse(&g_bodies[1], (fm_vec3_t){{ tx * 26.0f, 14.0f, tz * 26.0f }});
        if (g_audible) kiln_sound_play("thump", g_bodies[2].pos, 0.8f);
    }
    if (crossed(t0, t1, CINE_BUMP_B_T)) {
        kiln_physics_apply_impulse(&g_bodies[4], (fm_vec3_t){{ 6.0f, 26.0f, 38.0f }});
        if (g_audible) kiln_sound_play("thump", g_bodies[4].pos, 0.8f);
    }

    g_t = t1;
    kiln_event_process(dt);
    kiln_physics_step(&g_pworld, dt);
    crates_update(dt);
    kiln_actor_update_all(dt);

    if (wrapped) crates_reset();
}

// ── BOXES: measured bounds over the drawn models ───────────────────────
static void dd_bounds(fm_vec3_t pos, const CineBounds *b, color_t c)
{
    const float m = CINE_UNITS_PER_M;
    const float rx = (b->mx[0] > -b->mn[0] ? b->mx[0] : -b->mn[0]) * m;
    const float rz = (b->mx[2] > -b->mn[2] ? b->mx[2] : -b->mn[2]) * m;
    kiln_dd_aabb((fm_vec3_t){{ pos.v[0] - rx, pos.v[1] + b->mn[1] * m, pos.v[2] - rz }},
                 (fm_vec3_t){{ pos.v[0] + rx, pos.v[1] + b->mx[1] * m, pos.v[2] + rz }}, c);
}

static void draw_boxes(void)
{
    kiln_dd_begin(&g_scene, SCREEN_W, SCREEN_H);
    dd_bounds(g_ship_xf.pos, &CINE_B_SHIP, RGBA32(80, 220, 255, 255));
    for (KilnActor *a = kiln_actor_first(KILN_ACTOR_CAT_PLAYER); a; a = kiln_actor_next(a))
        dd_bounds(a->xform.pos, &CINE_B_GOBLIN, RGBA32(120, 255, 120, 255));
    for (KilnActor *a = kiln_actor_first(KILN_ACTOR_CAT_NPC); a; a = kiln_actor_next(a))
        dd_bounds(a->xform.pos, &CINE_B_DROID, RGBA32(255, 255, 90, 255));
    for (KilnActor *a = kiln_actor_first(KILN_ACTOR_CAT_ENEMY); a; a = kiln_actor_next(a))
        dd_bounds(a->xform.pos, &CINE_B_ALIEN, RGBA32(255, 90, 255, 255));
    kiln_dd_end();
}

// ── HUD: letterbox and caption cards ───────────────────────────────────
// Opaque bars, because kiln_gui draws with the blender off and would ignore
// any alpha. The picture between them is 320x184, a 1.74:1 frame. A shot's
// caption shows for its first three seconds, in the lower bar where it can
// never cover the subject.
#define LETTERBOX_H 28

static void draw_hud(void)
{
    const color_t bar = RGBA32(0, 0, 0, 255);
    kiln_gui_rect(0, 0, SCREEN_W, LETTERBOX_H, bar);
    kiln_gui_rect(0, SCREEN_H - LETTERBOX_H, SCREEN_W, LETTERBOX_H, bar);
    kiln_gui_text(12, 18, RGBA32(0, 245, 212, 255), "KILN ENGINE");
    kiln_gui_text(SCREEN_W - 12 - 6 * 11, 18, RGBA32(120, 128, 150, 255), "HANGAR BAY");

    const CineShot *s = &CINE_SHOTS[cine_shot_at(CINE_SHOTS, CINE_SHOT_COUNT, g_t)];
    if (s->title && g_t - s->start < 3.0f) {
        kiln_gui_rect(12, SCREEN_H - 20, 3, 11, RGBA32(245, 180, 60, 255));
        kiln_gui_text(20, SCREEN_H - 11, RGBA32(232, 232, 240, 255), "%s", s->title);
    }
    int sec = (int)g_t;
    kiln_gui_text(SCREEN_W - 12 - 6 * 5, SCREEN_H - 11, RGBA32(120, 128, 150, 255),
                  "00:%02d", sec);
}

int main(void)
{
    debug_init_isviewer();
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);
    asset_init_compression(2);

    kiln_input_init();
    kiln_audio_init(KILN_AUDIO_DEFAULT);

    // ── Sound ──────────────────────────────────────────────────────
    // Falloff at the scale of the room: a footstep across the hangar is
    // quiet, one under the camera is not.
    static const KilnSoundShader shaders[] = {
        { .name = "step_stone", .wav64_path = "rom:/sfx/step.wav64",
          .base_vol = 0.7f, .falloff_radius = 150.0f },
        { .name = "step_alien", .wav64_path = "rom:/sfx/step.wav64",
          .base_vol = 0.6f, .falloff_radius = 170.0f },
        { .name = "thump", .wav64_path = "rom:/sfx/blip.wav64",
          .base_vol = 0.6f, .falloff_radius = 220.0f },
    };
    kiln_sound_init(shaders, 3);

    kiln_actor_system_init(PROFILES, PROFILE_COUNT, g_pool, ACTOR_POOL_CAP);
    kiln_event_init();

    // ── Map ────────────────────────────────────────────────────────
    kiln_map_register_classname("info_player_start", PROFILE_GOBLIN);
    kiln_map_register_classname("info_droid",        PROFILE_DROID);
    kiln_map_register_classname("info_alien",        PROFILE_ALIEN);
    if (kiln_map_load(&g_map, "rom:/maps/hangar-map.map") != 0)
        debugf("cinematic-demo: kiln_map_load(hangar-map.map) failed\n");
    kiln_map_tint(&g_map, &(KilnMapTint){
        .floor = kiln_prim_rgba(0x8C, 0x90, 0x98), .floor_edge = kiln_prim_rgba(0x66, 0x6A, 0x74),
        .floor_y = 1.0f, .floor_radius = 140.0f,
        .top = kiln_prim_rgba(0xC8, 0xA0, 0x48),                  // the pad: hazard amber
        .wall_low = kiln_prim_rgba(0x44, 0x4A, 0x58), .wall_high = kiln_prim_rgba(0x96, 0xA0, 0xB4),
        .z_face_shade = 0.82f,
        .underside = kiln_prim_rgba(0x3A, 0x3E, 0x48),
    });
    kiln_clip_set_world(g_map.brushes, g_map.brush_count);

    // ── Models ─────────────────────────────────────────────────────
    g_ship_model   = t3d_model_load("rom:/models/interceptor.t3dm");
    g_goblin_model = t3d_model_load("rom:/models/goblin.t3dm");
    g_droid_model  = t3d_model_load("rom:/models/droid.t3dm");
    g_alien_model  = t3d_model_load("rom:/models/alien.t3dm");

    kiln_skel_create(&g_goblin_skel, g_goblin_model);
    kiln_skel_play(&g_goblin_skel, "Idle", true);
    kiln_skel_play_blend(&g_goblin_skel, "Walk", true);
    for (int i = 0; i < DROID_MAX; i++) {
        kiln_skel_create(&g_droid_skel[i], g_droid_model);
        kiln_skel_play(&g_droid_skel[i], "Wave", true);
        kiln_skel_play_blend(&g_droid_skel[i], "Idle", true);
    }
    for (int i = 0; i < ALIEN_MAX; i++) {
        kiln_skel_create(&g_alien_skel[i], g_alien_model);
        kiln_skel_play(&g_alien_skel[i], "Idle", true);
        kiln_skel_play_blend(&g_alien_skel[i], "Approach", true);
    }

    kiln_prim_box(&g_door_prim,
                  (fm_vec3_t){{ CINE_DOOR_W * 0.5f, CINE_DOOR_H * 0.5f, 0 }},
                  (fm_vec3_t){{ CINE_DOOR_W * 0.5f - 0.3f, CINE_DOOR_H * 0.5f - 0.2f, 1.5f }},
                  kiln_prim_rgba(0xE0, 0x9A, 0x3C), kiln_prim_rgba(0xB8, 0x74, 0x28),
                  kiln_prim_rgba(0x50, 0x34, 0x18));
    kiln_prim_box(&g_crate_prim, (fm_vec3_t){{ 0, 0, 0 }},
                  (fm_vec3_t){{ CINE_CRATE_HALF, CINE_CRATE_HALF, CINE_CRATE_HALF }},
                  kiln_prim_rgba(0xD8, 0xB0, 0x70), kiln_prim_rgba(0xA8, 0x7C, 0x44),
                  kiln_prim_rgba(0x5C, 0x40, 0x20));
    for (int i = 0; i < CRATE_MAX; i++) kiln_transform_init(&g_crate_xf[i]);

    // ── Actors ─────────────────────────────────────────────────────
    int nd = 0, na = 0;
    for (int i = 0; i < g_map.spawn_count; i++) {
        KilnRoomSpawn *s = &g_map.spawns[i];
        if (s->profile_id == PROFILE_GOBLIN && g_goblin_h == KILN_ACTOR_HANDLE_NONE)
            g_goblin_h = kiln_actor_spawn(s->profile_id, s->pos, s->yaw, &s->dict);
        else if (s->profile_id == PROFILE_DROID && nd < DROID_MAX)
            kiln_actor_spawn(s->profile_id, s->pos, s->yaw, &s->dict), nd++;
        else if (s->profile_id == PROFILE_ALIEN && na < ALIEN_MAX)
            g_alien_h[na++] = kiln_actor_spawn(s->profile_id, s->pos, s->yaw, &s->dict);
    }
    g_door_h = kiln_actor_spawn(PROFILE_DOOR,
        (fm_vec3_t){{ CINE_DOOR_HINGE_X, CINE_FLOOR_Y, CINE_DOOR_Z }}, 0.0f, NULL);
    debugf("cinematic-demo: brushes %d spawns %d droids %d aliens %d\n",
           g_map.brush_count, g_map.spawn_count, nd, na);

    kiln_transform_init(&g_ship_xf);
    g_ship_xf.pos = (fm_vec3_t){{ 0.0f, cine_stand_y(&CINE_B_SHIP, CINE_PAD_TOP), 0.0f }};
    g_ship_xf.scale = (fm_vec3_t){{ CINE_SCALE, CINE_SCALE, CINE_SCALE }};
    g_ship_xf.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
    g_ship_xf.rot_angle = CINE_PI;                    // nose toward the front wall (+Z)

    crates_reset();

    // ── Scene ──────────────────────────────────────────────────────
    // The hangar is 200 units across and the far corner of the corridor is
    // ~300 from the opposite corner, so that is the far plane; fog closes in
    // on the far walls so the room ends in haze rather than at an edge.
    kiln_scene_init(&g_scene);
    kiln_prim_stage(&g_scene, RGBA32(0x34, 0x3A, 0x4A, 0xFF), 160.0f, 340.0f);
    // Tiny3D's microcode lights a vertex by +dot(normal, dir): a direction
    // points TOWARD its light. Set here explicitly rather than inherited, so
    // the floor is lit by the overhead key on console whichever way the preset
    // happens to point. Key: high, over the front-right; rim: low, from the
    // back-left, cool.
    g_scene.light_dir = (fm_vec3_t){{ 0.35f, 0.85f, 0.40f }};
    fm_vec3_norm(&g_scene.light_dir, &g_scene.light_dir);
    g_scene.lights[0].dir = (fm_vec3_t){{ -0.60f, 0.35f, -0.70f }};
    fm_vec3_norm(&g_scene.lights[0].dir, &g_scene.lights[0].dir);
    g_scene.fov_deg = 62.0f;
    g_scene.near_z = 4.0f;
    g_scene.far_z = 360.0f;

    kiln_camera_init(&g_cam);
    kiln_camera_push(&g_cam, KILN_CAM_CUTSCENE);

    // ── Music bed ──────────────────────────────────────────────────
    g_music = kiln_sfx_load("rom:/sfx/cine_loop.wav64");
    if (g_music >= 0) {
        g_music_ch = kiln_sfx_play(g_music, -1, 0);
        if (g_music_ch >= 0) kiln_sfx_set_vol_pan(g_music_ch, 0.55f, 0.5f);
    }

    // A jump ROM runs the real timeline up to its second, silently and
    // undrawn: events, physics and actor updates all go through sim_step, so
    // the door has swung and the crates have landed exactly as they would
    // have. The loop below then holds time still.
    const float jump_t = jump_time();
    if (jump_t >= 0.0f) {
        g_audible = 0;
        for (int f = 0; f < (int)(jump_t * 60.0f + 0.5f); f++) sim_step(DT);
        g_t = jump_t;
        kiln_actor_update_all(0.0f);
        g_audible = 1;
    }

    for (;;) {
        kiln_input_update();
        if (jump_t < 0.0f) sim_step(DT);

        kiln_skel_update(&g_goblin_skel, DT);
        for (int i = 0; i < DROID_MAX; i++) kiln_skel_update(&g_droid_skel[i], DT);
        for (int i = 0; i < ALIEN_MAX; i++) kiln_skel_update(&g_alien_skel[i], DT);

        fm_vec3_t eye, look;
        cine_camera(CINE_SHOTS, CINE_SHOT_COUNT, g_t, &eye, &look);
        kiln_camera_set_cutscene(&g_cam, eye, look);
        kiln_camera_update(&g_cam, (fm_vec3_t){{ 0, 0, 0 }}, 0.0f, DT);
        kiln_camera_apply(&g_cam, &g_scene);
        kiln_scene_update(&g_scene);

        kiln_sound_update_listener(g_scene.cam_pos,
            (fm_vec3_t){{ look.v[0] - eye.v[0], 0, look.v[2] - eye.v[2] }});

        kiln_frame_begin();
        kiln_scene_begin(&g_scene);

        kiln_map_draw(&g_map);
        kiln_transform_push(&g_ship_xf);
        t3d_model_draw(g_ship_model);
        kiln_transform_pop();
        crates_draw();
        kiln_actor_draw_all();

        kiln_gui_begin();
        if (KILN_JUMP == JUMP_BOXES) draw_boxes();
        draw_hud();
        if (g_t >= 30.0f && g_t < 36.0f) {
            KilnActor *lead = kiln_actor_resolve(g_alien_h[0]);
            if (lead) {
                fm_vec3_t chest = lead->xform.pos;
                chest.v[1] += CINE_B_ALIEN.mx[1] * CINE_UNITS_PER_M * 0.6f;
                kiln_target_draw_reticle(&g_scene, chest, SCREEN_W, SCREEN_H,
                                         RGBA32(245, 64, 80, 255));
            }
        }
        kiln_gui_end();
        kiln_frame_end();

        kiln_sound_update();
        kiln_audio_update();
    }
}
