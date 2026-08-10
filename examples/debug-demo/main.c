// SPDX-License-Identifier: MPL-2.0
//
// debug-demo: the on-screen retro console (engine/src/m64/m64_console.*).
// A player cube and two orbiting enemies; the debug console is wired in
// behind the `M64_DEBUG` flag (set by `mkN64Rom { debugConsole = true; }`).
//
// Toggle the console by holding Start and pressing the C buttons
// counter-clockwise: C-Up, C-Left, C-Down, C-Right. While open, the stick
// moves the keyboard cursor, A types the highlighted cell, B backspaces,
// Start submits the command line. Built-ins: `help`, `clear`, `exit`. The
// demo registers `actors` and `fps` as well.
//
// Without M64_DEBUG, the console calls compile out and the ROM is a
// stripped-down actors demo — no behaviour change vs. examples/actors-demo
// beyond the smaller cast.

#include <libdragon.h>
#include <m64/m64_engine.h>
#include <m64/m64_gui.h>
#include <m64/m64_input.h>
#include <m64/m64_actor.h>
#include <m64/m64_console.h>
#include <m64/m64_panic.h>
#include <m64/m64_prof.h>

#include <malloc.h>

#define SCREEN_W 320
#define SCREEN_H 240
#define ACTOR_POOL_CAP 16

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

enum { PROFILE_PLAYER, PROFILE_ENEMY, PROFILE_COUNT };

static T3DVertPacked *g_cube_player;
static T3DVertPacked *g_cube_enemy;

typedef struct { float angle, radius, speed; } EnemyState;

static void player_update(M64Actor *self, float dt)
{
    /* When the console is open, skip gameplay input so the stick navigates
     * the keyboard, not the player. */
#ifdef M64_DEBUG
    if (m64_console_is_open()) return;
#endif
    const M64Input *in = m64_input_get(1);
    self->xform.pos.v[0] += in->stick_x * 4.0f * dt;
    self->xform.pos.v[2] -= in->stick_y * 4.0f * dt;
    self->xform.rot_angle += dt;
}
static void player_draw(M64Actor *self) { (void)self; draw_cube(g_cube_player); }

static void enemy_init(M64Actor *self, const M64Dict *spawn_args)
{
    (void)spawn_args;
    EnemyState *s = (EnemyState *)self->state;
    s->angle = 0.0f; s->radius = 40.0f; s->speed = 1.0f;
}
static void enemy_update(M64Actor *self, float dt)
{
    EnemyState *s = (EnemyState *)self->state;
    s->angle += s->speed * dt;
    self->xform.pos.v[0] = fm_cosf(s->angle) * s->radius;
    self->xform.pos.v[2] = fm_sinf(s->angle) * s->radius;
    self->xform.rot_angle = s->angle;
}
static void enemy_draw(M64Actor *self) { (void)self; draw_cube(g_cube_enemy); }

static const M64ActorProfile PROFILES[PROFILE_COUNT] = {
    [PROFILE_PLAYER] = { .name = "player", .category = M64_ACTOR_CAT_PLAYER,
                          .state_size = 0,
                          .update = player_update, .draw = player_draw },
    [PROFILE_ENEMY]  = { .name = "enemy", .category = M64_ACTOR_CAT_ENEMY,
                          .state_size = sizeof(EnemyState),
                          .init = enemy_init, .update = enemy_update,
                          .draw = enemy_draw },
};

static M64Actor g_pool[ACTOR_POOL_CAP];

// ── Console commands ─────────────────────────────────────────────────────
#ifdef M64_DEBUG
static void cmd_panic(int argc, const char **argv)
{
    (void)argc; (void)argv;
    m64_panic_message("user triggered panic via console\n");
    *(volatile int *)0 = 0; /* deliberate null dereference */
}

static const M64ConsoleCmd CMDS[] = {
    { "panic", "trigger a deliberate crash", cmd_panic },
};
#endif

float g_dbg_fps = 0.0f;

int main(void)
{
    m64_engine_init(RESOLUTION_320x240);
    joypad_init();
    m64_input_init();

    g_cube_player = make_color_cube(8, 0xFFD94CFF);
    g_cube_enemy  = make_color_cube(10, 0xFF4C6AFF);

    m64_actor_system_init(PROFILES, PROFILE_COUNT, g_pool, ACTOR_POOL_CAP);
    m64_actor_spawn(PROFILE_PLAYER, (fm_vec3_t){{ 0, 0, 0 }}, 0.0f, NULL);
    for (int i = 0; i < 2; i++) {
        M64ActorHandle h = m64_actor_spawn(PROFILE_ENEMY,
                                           (fm_vec3_t){{ 0, 0, 0 }}, 0.0f, NULL);
        M64Actor *a = m64_actor_resolve(h);
        EnemyState *s = (EnemyState *)a->state;
        s->radius = 30.0f + i * 15.0f;
        s->speed  = 0.6f + i * 0.3f;
        s->angle  = i * 2.1f;
    }

#ifdef M64_DEBUG
    m64_prof_init();
    m64_console_init();
    m64_console_register(CMDS, sizeof CMDS / sizeof CMDS[0]);
    m64_console_log("debug-demo ready");
    m64_console_log("hold Start + C-Up Left Down Right");
#endif

    M64Scene scene;
    m64_scene_init(&scene);
    scene.far_z = 300.0f;

    uint32_t frames = 0;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        float dt = 1.0f / 60.0f;
        M64_PROF_BEGIN(M64_PROF_UPDATE);

        m64_input_update();

#ifdef M64_DEBUG
        m64_console_update(1);
#endif

        m64_actor_update_all(dt);

        M64Actor *player = m64_actor_first(M64_ACTOR_CAT_PLAYER);
        if (player) {
            scene.cam_target = player->xform.pos;
            scene.cam_pos = (fm_vec3_t){{
                player->xform.pos.v[0],
                player->xform.pos.v[1] + 60.0f,
                player->xform.pos.v[2] - 110.0f,
            }};
        }
        m64_scene_update(&scene);

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            g_dbg_fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now)
                                 / TICKS_PER_SECOND);
            last_ticks = now;
        }

        M64_PROF_END(M64_PROF_UPDATE);
        m64_prof_frame_done();

        m64_frame_begin();
        M64_PROF_BEGIN(M64_PROF_SCENE);
        m64_scene_begin(&scene);
        m64_actor_draw_all();
        M64_PROF_END(M64_PROF_SCENE);

        m64_gui_begin();
        M64_PROF_BEGIN(M64_PROF_GUI);
        m64_gui_panel(8, 8, 180, 60,
                      RGBA32(10, 10, 24, 200), RGBA32(0, 245, 212, 255));
        m64_gui_text(14, 22, RGBA32(0, 245, 212, 255), "M64 DEBUG CONSOLE");
        m64_gui_text(14, 34, RGBA32(232, 232, 240, 255), "fps %5.1f", g_dbg_fps);
        m64_gui_text(14, 46, RGBA32(232, 232, 240, 255),
                     "player %u  enemy %u",
                     m64_actor_count(M64_ACTOR_CAT_PLAYER),
                     m64_actor_count(M64_ACTOR_CAT_ENEMY));
#ifdef M64_DEBUG
        M64_PROF_BEGIN(M64_PROF_DEBUG);
        m64_console_draw();
        M64_PROF_END(M64_PROF_DEBUG);
#endif
        M64_PROF_END(M64_PROF_GUI);
        m64_gui_end();
        m64_frame_end();
    }
}