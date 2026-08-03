// SPDX-License-Identifier: MPL-2.0
//
// Phase B completion: m64_camera (OoT-style spring-arm follow) + m64_skel
// (skeletal animation, idle/swing blend by movement speed) + m64_audio
// (footstep SFX gated on distance travelled), all driving the actor system
// examples/actors-demo already exercises. One skinned player actor (the
// 2-bone rig tools/gen_skel_gltf.py generates) walks around a handful of
// static prop cubes; the camera booms in behind it.
//
//   models/rig.t3dm  -> m64_skel, drawn skinned in the 3D pass
//   sfx/blip.wav64   -> m64_audio, one footstep every STEP_DISTANCE units

#include <libdragon.h>
#include <t3d/t3dmodel.h>
#include <m64/m64_engine.h>
#include <m64/m64_gui.h>
#include <m64/m64_actor.h>
#include <m64/m64_camera.h>
#include <m64/m64_skel.h>
#include <m64/m64_audio.h>

#include <malloc.h>

#define SCREEN_W 320
#define SCREEN_H 240
#define ACTOR_POOL_CAP 16
#define SAMPLE_RATE 32000

#define PLAYER_SPEED 50.0f      /* world units / second at full stick */
#define PLAYER_TURN_SPEED 8.0f  /* yaw damping, same shape as m64_camera's */
#define STEP_DISTANCE 24.0f     /* world units between footstep SFX */
#define STICK_DEADZONE 8.0f

static joypad_inputs_t g_input;
static M64Skel g_player_skel;
static int g_step_sfx = -1;

// ── Prop cubes: fixed landmarks so the camera's boom lag is visible ─────
// Same hand-built-cube approach as examples/actors-demo; this ROM is about
// camera/skel/audio, not the asset pipeline (that's assets-demo).
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
        fm_vec3_t na = {{ (float)c[i][0], (float)c[i][1], (float)c[i][2] }};
        fm_vec3_t nb = {{ (float)c[i+1][0], (float)c[i+1][1], (float)c[i+1][2] }};
        fm_vec3_norm(&na, &na);
        fm_vec3_norm(&nb, &nb);
        v[i / 2] = (T3DVertPacked){
            .posA = { c[i][0], c[i][1], c[i][2] }, .rgbaA = rgba,
            .normA = t3d_vert_pack_normal(&na),
            .posB = { c[i+1][0], c[i+1][1], c[i+1][2] }, .rgbaB = rgba,
            .normB = t3d_vert_pack_normal(&nb),
        };
    }
    return v;
}

static void draw_cube(T3DVertPacked *verts)
{
    t3d_vert_load(verts, 0, 8);
    for (int i = 0; i < 12; i++) t3d_tri_draw(CUBE_TRIS[i][0], CUBE_TRIS[i][1], CUBE_TRIS[i][2]);
    t3d_tri_sync();
}

// ── Actor profiles ───────────────────────────────────────────────────

enum { PROFILE_PLAYER, PROFILE_PROP, PROFILE_COUNT };

static T3DVertPacked *g_cube_prop;

typedef struct { float dist_since_step; float speed_norm; } PlayerState;

static void player_init(M64Actor *self)
{
    ((PlayerState *)self->state)->dist_since_step = 0.0f;
    ((PlayerState *)self->state)->speed_norm = 0.0f;
}

static void player_update(M64Actor *self, float dt)
{
    PlayerState *s = (PlayerState *)self->state;

    fm_vec3_t stick = {{ (float)g_input.stick_x, 0.0f, -(float)g_input.stick_y }};
    float mag = fm_vec3_len(&stick);

    if (mag > STICK_DEADZONE) {
        fm_vec3_t dir = {{ stick.v[0] / mag, 0.0f, stick.v[2] / mag }};
        float speed_norm = mag / 80.0f;
        if (speed_norm > 1.0f) speed_norm = 1.0f;
        s->speed_norm = speed_norm;

        float move = PLAYER_SPEED * speed_norm * dt;
        self->xform.pos.v[0] += dir.v[0] * move;
        self->xform.pos.v[2] += dir.v[2] * move;

        float target_yaw = fm_atan2f(dir.v[0], dir.v[2]);
        float t = PLAYER_TURN_SPEED * dt;
        if (t > 1.0f) t = 1.0f;
        self->xform.rot_angle = fm_lerp_angle(self->xform.rot_angle, target_yaw, t);

        s->dist_since_step += move;
        if (s->dist_since_step >= STEP_DISTANCE) {
            s->dist_since_step -= STEP_DISTANCE;
            if (g_step_sfx >= 0) m64_sfx_play(g_step_sfx, -1, 1);
        }
    } else {
        s->speed_norm = 0.0f;
    }

    // Blend factor is set here (per-actor logic) but not applied until
    // m64_skel_update runs later this frame in main() — see the file
    // comment on why the skeleton is a global, not per-instance, resource.
    m64_skel_set_blend(&g_player_skel, s->speed_norm);
}

static void player_draw(M64Actor *self)
{
    m64_transform_push(&self->xform);
    m64_skel_draw(&g_player_skel);
    m64_transform_pop();
}

static void prop_draw(M64Actor *self) { (void)self; draw_cube(g_cube_prop); }

static const M64ActorProfile PROFILES[PROFILE_COUNT] = {
    [PROFILE_PLAYER] = { .name = "player", .category = M64_ACTOR_CAT_PLAYER,
                          .state_size = sizeof(PlayerState),
                          .init = player_init, .update = player_update, .draw = player_draw },
    [PROFILE_PROP]   = { .name = "prop", .category = M64_ACTOR_CAT_PROP,
                          .state_size = 0, .draw = prop_draw },
};

static M64Actor g_pool[ACTOR_POOL_CAP];

int main(void)
{
    m64_engine_init(RESOLUTION_320x240);
    joypad_init();

    dfs_init(DFS_DEFAULT_LOCATION);
    asset_init_compression(2); // matches mkModel's `mkasset -c 2` (see assets-demo)

    m64_audio_init(M64_AUDIO_DEFAULT);
    g_step_sfx = m64_sfx_load("rom:/sfx/blip.wav64");

    T3DModel *rig = t3d_model_load("rom:/models/rig.t3dm");
    m64_skel_create(&g_player_skel, rig);
    m64_skel_play(&g_player_skel, "idle", true);
    m64_skel_play_blend(&g_player_skel, "swing", true);

    g_cube_prop = make_color_cube(14, 0x00F5D4FF);

    m64_actor_system_init(PROFILES, PROFILE_COUNT, g_pool, ACTOR_POOL_CAP);
    M64ActorHandle player = m64_actor_spawn(PROFILE_PLAYER, (fm_vec3_t){{ 0, 0, 0 }}, 0.0f);

    static const fm_vec3_t prop_pos[6] = {
        {{ 100, 0, 100 }}, {{ -100, 0, 100 }}, {{ 100, 0, -100 }},
        {{ -100, 0, -100 }}, {{ 0, 0, 160 }}, {{ 0, 0, -160 }},
    };
    for (int i = 0; i < 6; i++) m64_actor_spawn(PROFILE_PROP, prop_pos[i], 0.0f);

    M64Scene scene;
    m64_scene_init(&scene);
    scene.far_z = 400.0f;

    M64Camera cam;
    m64_camera_init(&cam);
    cam.distance = 90.0f;
    cam.height = 55.0f;
    cam.look_height = 30.0f;

    M64Actor *p0 = m64_actor_resolve(player);
    m64_camera_snap(&cam, p0->xform.pos, p0->xform.rot_angle);

    uint32_t frames = 0;
    float fps = 0.0f;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        joypad_poll();
        g_input = joypad_get_inputs(JOYPAD_PORT_1);

        float dt = 1.0f / 60.0f;
        m64_actor_update_all(dt);
        m64_skel_update(&g_player_skel, dt);

        M64Actor *pl = m64_actor_resolve(player);
        m64_camera_update(&cam, pl->xform.pos, pl->xform.rot_angle, dt);
        m64_camera_apply(&cam, &scene);
        m64_scene_update(&scene);

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        m64_frame_begin();
        m64_scene_begin(&scene);
        m64_actor_draw_all();

        m64_gui_begin();
        m64_gui_panel(8, 8, 170, 58,
                      RGBA32(10, 10, 24, 200), RGBA32(0, 245, 212, 255));
        m64_gui_text(14, 22, RGBA32(0, 245, 212, 255), "M64 CAMERA + SKEL");
        m64_gui_text(14, 34, RGBA32(232, 232, 240, 255), "fps %5.1f", fps);
        PlayerState *ps = (PlayerState *)pl->state;
        m64_gui_text(14, 46, RGBA32(232, 232, 240, 255), "speed %4.2f  yaw %5.2f",
                     ps->speed_norm, cam.yaw);
        m64_gui_text(14, 58, RGBA32(232, 232, 240, 255), "stick: move + swing arm");
        m64_gui_end();

        m64_frame_end();
        m64_audio_update();
    }
}
