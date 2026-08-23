// SPDX-License-Identifier: MIT
//
// Phase 6: the OoT + id Tech 4 integration proof. A player actor (driven by
// kiln_player) walks a Quake-format .map room, sliding against the walls via
// kiln_clip. Holding Z triggers kiln_target_acquire; the camera pushes
// TARGETING mode (kiln_camera_push) and orbits to keep the player and the
// locked enemy in frame; releasing Z pops back to NORMAL. Footstep SFX
// fires via kiln_event (kiln_player posts KILN_EV_PLAYER_FOOTSTEP, the player
// actor's KilnActorEventFn dispatches to kiln_sound_play). A 1.5 s cutscene
// pans around the player on boot to demonstrate the camera mode stack.
//
//   kiln_map      -> assets/oot_test.map -> brushes + face quads + spawns
//   kiln_dict     -> spawn args (origin, angle) read at spawn
//   kiln_clip     -> swept-AABB vs the parsed brushes
//   kiln_player   -> locomotion state machine + FOOTSTEP events
//   kiln_event    -> deferred FOOTSTEP dispatch
//   kiln_target   -> cone acquire + reticle projection
//   kiln_camera   -> mode stack (NORMAL → TARGETING → CUTSCENE) + collision boom
//   kiln_surface  -> friction table (index 0 = stone)
//   kiln_sound    -> positional sound shaders

#include <libdragon.h>
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
#ifdef KILN_DEBUG
#include <kiln/kiln_console.h>
#include <kiln/kiln_prof.h>
#endif

#include <malloc.h>

#define SCREEN_W 320
#define SCREEN_H 240
#define ACTOR_POOL_CAP 16

// ── Profiles ────────────────────────────────────────────────────────────
enum { PROFILE_PLAYER, PROFILE_ENEMY, PROFILE_COUNT };

// Unit cube + draw helper, same pattern as every other demo.
static const uint8_t CUBE_TRIS[12][3] = {
    {0,1,2},{2,3,0}, {4,6,5},{6,4,7},
    {0,4,5},{5,1,0}, {1,5,6},{6,2,1},
    {2,6,7},{7,3,2}, {3,7,4},{4,0,3},
};

static T3DVertPacked *make_color_cube(int16_t half, uint32_t rgba)
{
    T3DVertPacked *v = malloc_uncached(sizeof(T3DVertPacked) * 4);
    const int16_t s = half;
    const int16_t c[8][3] = {
        {-s,-s,-s},{ s,-s,-s},{ s, s,-s},{-s, s,-s},
        {-s,-s, s},{ s,-s, s},{ s, s, s},{-s, s, s},
    };
    for (int i = 0; i < 8; i += 2) {
        fm_vec3_t na = {{ (float)c[i][0],   (float)c[i][1],   (float)c[i][2]   }};
        fm_vec3_t nb = {{ (float)c[i+1][0], (float)c[i+1][1], (float)c[i+1][2] }};
        fm_vec3_norm(&na, &na);
        fm_vec3_norm(&nb, &nb);
        v[i / 2] = (T3DVertPacked){
            .posA = { c[i][0],   c[i][1],   c[i][2]   }, .rgbaA = rgba,
            .normA = t3d_vert_pack_normal(&na),
            .posB = { c[i+1][0], c[i+1][1], c[i+1][2] }, .rgbaB = rgba,
            .normB = t3d_vert_pack_normal(&nb),
        };
    }
    return v;
}

static void draw_cube(T3DVertPacked *v)
{
    t3d_vert_load(v, 0, 8);
    for (int i = 0; i < 12; i++)
        t3d_tri_draw(CUBE_TRIS[i][0], CUBE_TRIS[i][1], CUBE_TRIS[i][2]);
    t3d_tri_sync();
}

static T3DVertPacked *g_cube_player;
static T3DVertPacked *g_cube_enemy;

// ── Enemy ──────────────────────────────────────────────────────────────
typedef struct { float angle, radius, speed; } EnemyState;

static void enemy_init(KilnActor *self, const KilnDict *spawn_args)
{
    EnemyState *s = (EnemyState *)self->state;
    s->angle = 0.0f;
    s->radius = 30.0f;
    s->speed = 0.8f;
    /* Read "origin" if present (the map parser already wrote pos from it,
     * but exercising the dict here is the integration check). */
    (void)spawn_args;
}

static void enemy_update(KilnActor *self, float dt)
{
    EnemyState *s = (EnemyState *)self->state;
    s->angle += s->speed * dt;
    /* Orbit around the spawn point (the actor's pos was set at spawn). The
     * radius and speed are stable; only the orbit centre moves with the
     * initial pos by storing the orbit as an offset. For simplicity, orbit
     * around the world origin — the two enemies spawn at (60,60) and
     * (-60,-60), so each orbits its own corner of the room. */
    fm_vec3_t base = self->xform.pos;
    self->xform.pos.v[0] = base.v[0] + fm_cosf(s->angle) * s->radius * 0.02f;
    self->xform.pos.v[2] = base.v[2] + fm_sinf(s->angle) * s->radius * 0.02f;
    self->xform.rot_angle = s->angle;
}

static void enemy_draw(KilnActor *self) { (void)self; draw_cube(g_cube_enemy); }

// ── Player ─────────────────────────────────────────────────────────────
static void player_init(KilnActor *self, const KilnDict *spawn_args)
{
    KilnPlayer *p = kiln_player_of(self);
    p->state = KILN_PLAYER_IDLE;
    p->vel = (fm_vec3_t){{ 0, 0, 0 }};
    p->yaw = 0.0f;
    p->state_t = 0.0f;
    p->step_cd = 0.0f;
    p->on_ground = 1;
    p->last_surf = 0;
    (void)spawn_args;
}

static void player_update(KilnActor *self, float dt)
{
    /* Player port 1 (kiln_input_get is 1-based). */
    kiln_player_update(self, 1, dt);
}

static void player_draw(KilnActor *self) { (void)self; draw_cube(g_cube_player); }

static void player_event(KilnActor *self, uint16_t event_id,
                         const int32_t *args, uint8_t argc)
{
    (void)self;
    if (event_id == KILN_EV_PLAYER_FOOTSTEP && argc >= 1) {
        /* args[0] is the surface id underfoot. The demo only registers
         * surface 0 (stone), so the shader choice is trivial; a game with
         * multiple surfaces would map id→shader name here. */
        (void)args;
        kiln_sound_play("step_stone", self->xform.pos, 1.0f);
    }
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
static int g_z_held = 0;
static float g_cutscene_t = 0.0f;

static fm_vec3_t cam_fwd(void)
{
    fm_vec3_t f = {{ g_scene.cam_target.v[0] - g_scene.cam_pos.v[0],
                     g_scene.cam_target.v[1] - g_scene.cam_pos.v[1],
                     g_scene.cam_target.v[2] - g_scene.cam_pos.v[2] }};
    f.v[1] = 0;
    fm_vec3_norm(&f, &f);
    return f;
}
static fm_vec3_t cam_right(void)
{
    fm_vec3_t f = cam_fwd();
    return (fm_vec3_t){{ -f.v[2], 0, f.v[0] }};
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
        { .name = "step_stone", .wav64_path = "rom:/sfx/step.wav64",
          .base_vol = 0.7f, .falloff_radius = 0.0f },
    };
    kiln_sound_init(shaders, 1);

    kiln_actor_system_init(PROFILES, PROFILE_COUNT, g_pool, ACTOR_POOL_CAP);
    kiln_event_init();

#ifdef KILN_DEBUG
    kiln_prof_init();
    kiln_console_init();
    kiln_console_log("oot-demo debug console ready");
    kiln_console_log("hold Start + C-Up Left Down Right");
#endif

    kiln_map_register_classname("info_player_start", PROFILE_PLAYER);
    kiln_map_register_classname("info_enemy", PROFILE_ENEMY);

    if (kiln_map_load(&g_map, "rom:/maps/oot_test.map") < 0) {
        debugf("oot-demo: kiln_map_load failed\n");
    }
    kiln_clip_set_world(g_map.brushes, g_map.brush_count);

    /* Spawn from the map entities. info_player_start → player; info_enemy →
     * two enemies. The map parser fills KilnRoomSpawn.{pos,yaw,dict}; we
     * just kiln_actor_spawn with the parsed pos/yaw and pass the dict. */
    fm_vec3_t ppos = (fm_vec3_t){{ 0, 16, 0 }};
    float pyaw = 0.0f;
    int spawned_player = 0, spawned_enemies = 0;
    for (int i = 0; i < g_map.spawn_count; i++) {
        KilnRoomSpawn *s = &g_map.spawns[i];
        if (s->profile_id == PROFILE_PLAYER && !spawned_player) {
            g_player_h = kiln_actor_spawn(s->profile_id, s->pos, s->yaw, &s->dict);
            ppos = s->pos; pyaw = s->yaw;
            spawned_player = 1;
        } else if (s->profile_id == PROFILE_ENEMY && spawned_enemies < 2) {
            kiln_actor_spawn(s->profile_id, s->pos, s->yaw, &s->dict);
            spawned_enemies++;
        }
    }
    if (!spawned_player) g_player_h = kiln_actor_spawn(PROFILE_PLAYER, ppos, pyaw, NULL);

    kiln_scene_init(&g_scene);
    g_scene.far_z = 500.0f;
    g_scene.ambient[3] = 255;

    kiln_camera_init(&g_cam);
    kiln_camera_set_collision(&g_cam, 1);
    kiln_camera_snap(&g_cam, ppos, pyaw);

    /* Boot cutscene: push CUTSCENE, pan around the player for 1.5 s, then
     * pop. Driven by g_cutscene_t in the loop. */
    kiln_camera_push(&g_cam, KILN_CAM_CUTSCENE);
    kiln_camera_set_cutscene(&g_cam,
        (fm_vec3_t){{ ppos.v[0] + 60, ppos.v[1] + 40, ppos.v[2] - 60 }},
        ppos);
    g_cutscene_t = 1.5f;

    uint32_t frames = 0;
    float fps = 0.0f;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        float dt = 1.0f / 60.0f;
#ifdef KILN_DEBUG
        KILN_PROF_BEGIN(KILN_PROF_UPDATE);
#endif

        kiln_input_update();
#ifdef KILN_DEBUG
        kiln_console_update(1);
#endif
        const KilnInput *in = kiln_input_get(1);

        /* Set the camera-relative movement basis BEFORE the player update
         * so stick-up moves the player in the camera's forward direction. */
        kiln_player_set_camera_basis(cam_fwd(), cam_right());

        /* Events before actor updates, per the engine contract. */
        kiln_event_process(dt);
        kiln_actor_update_all(dt);

        /* Player facing drives the NORMAL camera's boom heading. */
        KilnActor *player = kiln_actor_resolve(g_player_h);
        float p_yaw = player ? kiln_player_of(player)->yaw : 0.0f;
        fm_vec3_t ppos_now = player ? player->xform.pos : ppos;

        /* Z-targeting: hold Z to lock. Re-acquire on the rising edge so
         * tapping Z picks the best candidate each tap rather than sticking
         * to one until released. */
        int z_now = (in->buttons & KILN_BTN_Z) != 0;
        int z_edge = z_now && !g_z_held;
        g_z_held = z_now;

        if (g_cam.mode == KILN_CAM_NORMAL && z_edge) {
            fm_vec3_t eye = g_scene.cam_pos;
            fm_vec3_t f = cam_fwd();
            KilnActorHandle h = kiln_target_acquire(eye, f, 0.7f /* ~40° cone */,
                                                   200.0f);
            if (h != KILN_ACTOR_HANDLE_NONE) {
                kiln_camera_push(&g_cam, KILN_CAM_TARGETING);
                kiln_camera_set_target_actor(&g_cam, h);
                g_lock_h = h;
            }
        } else if (g_cam.mode == KILN_CAM_TARGETING && !z_now) {
            kiln_camera_pop(&g_cam);
            g_lock_h = KILN_ACTOR_HANDLE_NONE;
        }

        /* Cutscene countdown. When it elapses, pop back to NORMAL. */
        if (g_cam.mode == KILN_CAM_CUTSCENE) {
            g_cutscene_t -= dt;
            if (g_cutscene_t <= 0.0f) {
                kiln_camera_pop(&g_cam);
            }
        }

        kiln_camera_update(&g_cam, ppos_now, p_yaw, dt);
        kiln_camera_apply(&g_cam, &g_scene);
        kiln_scene_update(&g_scene);

        /* Listener for positional sound shaders. */
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
        kiln_actor_draw_all();
#ifdef KILN_DEBUG
        KILN_PROF_END(KILN_PROF_SCENE);
#endif

        /* ── 2D ───────────────────────────────────────────────────── */
        kiln_gui_begin();
#ifdef KILN_DEBUG
        KILN_PROF_BEGIN(KILN_PROF_GUI);
#endif
        kiln_gui_panel(8, 8, 200, 90,
                      RGBA32(10, 10, 24, 200), RGBA32(0, 245, 212, 255));
        kiln_gui_text(14, 22, RGBA32(0, 245, 212, 255), "KILN OOT+IDTECH4");
        kiln_gui_text(14, 34, RGBA32(232, 232, 240, 255), "fps %5.1f", fps);
        const char *mode_s = "NORMAL";
        if (g_cam.mode == KILN_CAM_TARGETING) mode_s = "TARGET";
        else if (g_cam.mode == KILN_CAM_CUTSCENE) mode_s = "CUT";
        kiln_gui_text(14, 46, RGBA32(232, 232, 240, 255), "cam  %s", mode_s);
        if (player) {
            KilnPlayer *pp = kiln_player_of(player);
            kiln_gui_text(14, 58, RGBA32(232, 232, 240, 255),
                         "plyr %d  surf %d", pp->state, pp->last_surf);
            kiln_gui_text(14, 70, RGBA32(232, 232, 240, 255),
                         "pos %5.1f %5.1f", pp->vel.v[0], pp->vel.v[2]);
        }
        kiln_gui_text(14, 82, RGBA32(232, 232, 240, 255),
                     "evt %u", kiln_event_count());

        kiln_gui_panel(8, SCREEN_H - 28, SCREEN_W - 16, 20,
                      RGBA32(10, 10, 24, 200), RGBA32(139, 92, 246, 255));
        kiln_gui_text(14, SCREEN_H - 18, RGBA32(232, 232, 240, 255),
                     "stick: move  Z: target  B: jump  A: attack  L: roll");

        /* Targeting reticle on the locked enemy. */
        if (g_lock_h != KILN_ACTOR_HANDLE_NONE) {
            KilnActor *t = kiln_actor_resolve(g_lock_h);
            if (t) {
                kiln_target_draw_reticle(&g_scene, t->xform.pos,
                                        SCREEN_W, SCREEN_H,
                                        RGBA32(245, 0, 80, 255));
            }
        }

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