// SPDX-License-Identifier: MIT
//
// debug-demo: the on-screen retro console (engine/src/kiln/kiln_console.*).
// A player cube and two orbiting enemies; the debug console is wired in
// behind the `KILN_DEBUG` flag (set by `mkN64Rom { debugConsole = true; }`).
//
// Toggle the console by holding Start and pressing the C buttons
// counter-clockwise: C-Up, C-Left, C-Down, C-Right. While open, the stick
// moves the keyboard cursor, A types the highlighted cell, B backspaces,
// Start submits the command line. Built-ins: `help`, `clear`, `exit`. The
// demo registers `actors` and `fps` as well.
//
// Without KILN_DEBUG, the console calls compile out and the ROM is a
// stripped-down actors demo — no behaviour change vs. examples/actors-demo
// beyond the smaller cast.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_actor.h>
#include <kiln/kiln_console.h>
#include <kiln/kiln_panic.h>
#include <kiln/kiln_prof.h>

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

static void player_update(KilnActor *self, float dt)
{
    /* When the console is open, skip gameplay input so the stick navigates
     * the keyboard, not the player. */
#ifdef KILN_DEBUG
    if (kiln_console_is_open()) return;
#endif
    const KilnInput *in = kiln_input_get(1);
    self->xform.pos.v[0] += in->stick_x * 4.0f * dt;
    self->xform.pos.v[2] -= in->stick_y * 4.0f * dt;
    self->xform.rot_angle += dt;
}
static void player_draw(KilnActor *self) { (void)self; draw_cube(g_cube_player); }

static void enemy_init(KilnActor *self, const KilnDict *spawn_args)
{
    (void)spawn_args;
    EnemyState *s = (EnemyState *)self->state;
    s->angle = 0.0f; s->radius = 40.0f; s->speed = 1.0f;
}
static void enemy_update(KilnActor *self, float dt)
{
    EnemyState *s = (EnemyState *)self->state;
    s->angle += s->speed * dt;
    self->xform.pos.v[0] = fm_cosf(s->angle) * s->radius;
    self->xform.pos.v[2] = fm_sinf(s->angle) * s->radius;
    self->xform.rot_angle = s->angle;
}
static void enemy_draw(KilnActor *self) { (void)self; draw_cube(g_cube_enemy); }

static const KilnActorProfile PROFILES[PROFILE_COUNT] = {
    [PROFILE_PLAYER] = { .name = "player", .category = KILN_ACTOR_CAT_PLAYER,
                          .state_size = 0,
                          .update = player_update, .draw = player_draw },
    [PROFILE_ENEMY]  = { .name = "enemy", .category = KILN_ACTOR_CAT_ENEMY,
                          .state_size = sizeof(EnemyState),
                          .init = enemy_init, .update = enemy_update,
                          .draw = enemy_draw },
};

static KilnActor g_pool[ACTOR_POOL_CAP];

// ── Console commands ─────────────────────────────────────────────────────
#ifdef KILN_DEBUG
static void cmd_panic(int argc, const char **argv)
{
    (void)argc; (void)argv;
    kiln_panic_message("user triggered panic via console\n");
    *(volatile int *)0 = 0; /* deliberate null dereference */
}

static const KilnConsoleCmd CMDS[] = {
    { "panic", "trigger a deliberate crash", cmd_panic },
};
#endif

float g_dbg_fps = 0.0f;

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    kiln_input_init();

    g_cube_player = make_color_cube(8, 0xFFD94CFF);
    g_cube_enemy  = make_color_cube(10, 0xFF4C6AFF);

    kiln_actor_system_init(PROFILES, PROFILE_COUNT, g_pool, ACTOR_POOL_CAP);
    kiln_actor_spawn(PROFILE_PLAYER, (fm_vec3_t){{ 0, 0, 0 }}, 0.0f, NULL);
    for (int i = 0; i < 2; i++) {
        KilnActorHandle h = kiln_actor_spawn(PROFILE_ENEMY,
                                           (fm_vec3_t){{ 0, 0, 0 }}, 0.0f, NULL);
        KilnActor *a = kiln_actor_resolve(h);
        EnemyState *s = (EnemyState *)a->state;
        s->radius = 30.0f + i * 15.0f;
        s->speed  = 0.6f + i * 0.3f;
        s->angle  = i * 2.1f;
    }

#ifdef KILN_DEBUG
    kiln_prof_init();
    kiln_console_init();
    kiln_console_register(CMDS, sizeof CMDS / sizeof CMDS[0]);
    kiln_console_log("debug-demo ready");
    kiln_console_log("hold Start + C-Up Left Down Right");
#endif

    KilnScene scene;
    kiln_scene_init(&scene);
    scene.far_z = 300.0f;

    uint32_t frames = 0;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        float dt = 1.0f / 60.0f;
        KILN_PROF_BEGIN(KILN_PROF_UPDATE);

        kiln_input_update();

#ifdef KILN_DEBUG
        kiln_console_update(1);
#endif

        kiln_actor_update_all(dt);

        KilnActor *player = kiln_actor_first(KILN_ACTOR_CAT_PLAYER);
        if (player) {
            scene.cam_target = player->xform.pos;
            scene.cam_pos = (fm_vec3_t){{
                player->xform.pos.v[0],
                player->xform.pos.v[1] + 60.0f,
                player->xform.pos.v[2] - 110.0f,
            }};
        }
        kiln_scene_update(&scene);

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            g_dbg_fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now)
                                 / TICKS_PER_SECOND);
            last_ticks = now;
        }

        KILN_PROF_END(KILN_PROF_UPDATE);
        kiln_prof_frame_done();

        kiln_frame_begin();
        KILN_PROF_BEGIN(KILN_PROF_SCENE);
        kiln_scene_begin(&scene);
        kiln_actor_draw_all();
        KILN_PROF_END(KILN_PROF_SCENE);

        kiln_gui_begin();
        KILN_PROF_BEGIN(KILN_PROF_GUI);
        kiln_gui_panel(8, 8, 180, 60,
                      RGBA32(10, 10, 24, 200), RGBA32(0, 245, 212, 255));
        kiln_gui_text(14, 22, RGBA32(0, 245, 212, 255), "KILN DEBUG CONSOLE");
        kiln_gui_text(14, 34, RGBA32(232, 232, 240, 255), "fps %5.1f", g_dbg_fps);
        kiln_gui_text(14, 46, RGBA32(232, 232, 240, 255),
                     "player %u  enemy %u",
                     kiln_actor_count(KILN_ACTOR_CAT_PLAYER),
                     kiln_actor_count(KILN_ACTOR_CAT_ENEMY));
#ifdef KILN_DEBUG
        KILN_PROF_BEGIN(KILN_PROF_DEBUG);
        kiln_console_draw();
        KILN_PROF_END(KILN_PROF_DEBUG);
#endif
        KILN_PROF_END(KILN_PROF_GUI);
        kiln_gui_end();
        kiln_frame_end();
    }
}