// SPDX-License-Identifier: MIT
//
// debug-demo: the engine's debugging kit, all on screen at once.
//
//   fig_console    the retro on-screen console. Hold Start and press the C
//                   buttons counter-clockwise (C-Up, C-Left, C-Down, C-Right)
//                   to open it; the stick moves the keyboard cursor, A types,
//                   B backspaces, Start submits. Commands: help, clear, exit,
//                   plus actors, fps, spawn, overlay, prof and panic.
//   fig_debugdraw  world-space overlays drawn in the 2D pass: each enemy's
//                   bounds and orbit, the world axes, labels — the spatial
//                   state that is otherwise only numbers in a HUD
//   fig_prof       per-zone CPU time, drawn as bars
//   fig_panic      `panic` dereferences NULL, to show the crash screen
//
// The console is compiled in by `mkN64Rom { debugConsole = true; }` (the
// KILN_DEBUG define); the debugdraw and profiler calls are always present, as
// kiln_console.h's house pattern prescribes.
//
//   stick walk   Z overlay   idle 2 s: the demo walks about
//
// Jump ROM: .#debug-demo-console enters the chord on its own, so the open
// console is on screen with no controller.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_actor.h>
#include <kiln/kiln_console.h>
#include <kiln/kiln_panic.h>
#include <kiln/kiln_prof.h>
#include <kiln/kiln_prim.h>
#include <kiln/kiln_debugdraw.h>

enum { JUMP_NONE, JUMP_CONSOLE };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W 320
#define SCREEN_H 240
#define ACTOR_POOL_CAP 16
#define ORBIT_POINTS 24

enum { PROFILE_PLAYER, PROFILE_ENEMY, PROFILE_COUNT };

static FigPrim g_body, g_nose, g_enemy, g_enemy_eye;
static int g_overlay = 1;
static float g_dbg_fps = 60.0f;

// ── Player ──────────────────────────────────────────────────────────────
static void player_init(FigActor *self, const FigDict *args)
{
    (void)args;
    self->xform.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
}

static void player_update(FigActor *self, float dt)
{
    /* While the console is open the stick drives its keyboard, not the player. */
#ifdef KILN_DEBUG
    if (fig_console_is_open()) return;
#endif
    const FigInput *in = fig_input_get(1);
    const float dx = -in->stick_x * 80.0f * dt, dz = in->stick_y * 80.0f * dt;
    self->xform.pos.v[0] += dx;
    self->xform.pos.v[2] += dz;
    if (dx * dx + dz * dz > 1e-4f) self->xform.rot_angle = fm_atan2f(dx, dz);
}

static void player_draw(FigActor *self) { (void)self; fig_prim_draw(&g_body); fig_prim_draw(&g_nose); }

// ── Enemy: orbits the point it spawned at ───────────────────────────────
typedef struct { float angle, radius, speed; fm_vec3_t centre; } EnemyState;

static void enemy_init(FigActor *self, const FigDict *args)
{
    (void)args;
    EnemyState *s = (EnemyState *)self->state;
    *s = (EnemyState){ .radius = 26.0f, .speed = 1.0f, .centre = self->xform.pos };
    self->xform.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
}

static void enemy_update(FigActor *self, float dt)
{
    EnemyState *s = (EnemyState *)self->state;
    s->angle += s->speed * dt;
    /* Around its own spawn point. The old demo orbited the world origin, so
     * both enemies drifted out of frame as soon as the player walked away. */
    self->xform.pos.v[0] = s->centre.v[0] + fm_cosf(s->angle) * s->radius;
    self->xform.pos.v[2] = s->centre.v[2] + fm_sinf(s->angle) * s->radius;
    self->xform.rot_angle = -s->angle;
}

static void enemy_draw(FigActor *self) { (void)self; fig_prim_draw(&g_enemy); fig_prim_draw(&g_enemy_eye); }

static const FigActorProfile PROFILES[PROFILE_COUNT] = {
    [PROFILE_PLAYER] = { .name = "player", .category = FIG_ACTOR_CAT_PLAYER,
                         .init = player_init, .update = player_update, .draw = player_draw },
    [PROFILE_ENEMY]  = { .name = "enemy", .category = FIG_ACTOR_CAT_ENEMY,
                         .state_size = sizeof(EnemyState),
                         .init = enemy_init, .update = enemy_update, .draw = enemy_draw },
};

static FigActor g_pool[ACTOR_POOL_CAP];

static void spawn_enemy(float x, float z, float radius, float speed, float phase)
{
    FigActor *a = fig_actor_resolve(fig_actor_spawn(PROFILE_ENEMY, (fm_vec3_t){{ x, 10, z }}, 0, NULL));
    if (!a) return;
    EnemyState *s = (EnemyState *)a->state;
    s->radius = radius; s->speed = speed; s->angle = phase;
}

// ── Console commands ────────────────────────────────────────────────────
#ifdef KILN_DEBUG
static void cmd_actors(int argc, const char **argv)
{
    (void)argc; (void)argv;
    fig_console_log("player %u enemy %u total %u", fig_actor_count(FIG_ACTOR_CAT_PLAYER),
                     fig_actor_count(FIG_ACTOR_CAT_ENEMY), fig_actor_count(FIG_ACTOR_CATEGORY_COUNT));
}

static void cmd_fps(int argc, const char **argv)
{
    (void)argc; (void)argv;
    fig_console_log("fps %.1f  frame %.2f ms", g_dbg_fps, fig_prof_ms(FIG_PROF_TOTAL));
}

static void cmd_spawn(int argc, const char **argv)
{
    (void)argc; (void)argv;
    FigActor *p = fig_actor_first(FIG_ACTOR_CAT_PLAYER);
    const float x = p ? p->xform.pos.v[0] : 0, z = p ? p->xform.pos.v[2] + 50 : 50;
    spawn_enemy(x, z, 18.0f, 1.6f, 0.0f);
    fig_console_log("spawned an enemy at %.0f %.0f", x, z);
}

static void cmd_overlay(int argc, const char **argv)
{
    (void)argc; (void)argv;
    g_overlay = !g_overlay;
    fig_console_log("overlay %s", g_overlay ? "on" : "off");
}

static void cmd_prof(int argc, const char **argv)
{
    (void)argc; (void)argv;
    fig_prof_print();
}

static void cmd_panic(int argc, const char **argv)
{
    (void)argc; (void)argv;
    fig_panic_message("user triggered panic via console\n");
    *(volatile int *)0 = 0; /* deliberate null dereference */
}

static const FigConsoleCmd CMDS[] = {
    { "actors",  "count actors by category",    cmd_actors },
    { "fps",     "print frame rate and time",   cmd_fps },
    { "spawn",   "spawn an enemy ahead of you", cmd_spawn },
    { "overlay", "toggle the debugdraw layer",  cmd_overlay },
    { "prof",    "log every profiler zone",     cmd_prof },
    { "panic",   "trigger a deliberate crash",  cmd_panic },
};
#endif

// ── Tapes ───────────────────────────────────────────────────────────────
static const FigInputKey WANDER_KEYS[] = {
    { .frame =   0, .sy =  80 },
    { .frame =  60, .sx = -70, .sy = 30 },
    { .frame = 130, .sy = -80 },
    { .frame = 200, .sx =  70, .sy = -20 },
    { .frame = 270, .sx =  30, .sy =  70 },
    { .frame = 330 },
    { .frame = 360 },
};
static const FigInputTape WANDER = { WANDER_KEYS, 7, 0 };

/* Start held throughout; C-Up, C-Left, C-Down, C-Right pressed in turn. */
static const FigInputKey CHORD_KEYS[] = {
    { .frame =  0, .buttons = FIG_BTN_START },
    { .frame = 10, .buttons = FIG_BTN_START | FIG_BTN_CU },
    { .frame = 14, .buttons = FIG_BTN_START },
    { .frame = 18, .buttons = FIG_BTN_START | FIG_BTN_CL },
    { .frame = 22, .buttons = FIG_BTN_START },
    { .frame = 26, .buttons = FIG_BTN_START | FIG_BTN_CD },
    { .frame = 30, .buttons = FIG_BTN_START },
    { .frame = 34, .buttons = FIG_BTN_START | FIG_BTN_CR },
    { .frame = 38 },
};
static const FigInputTape CHORD = { CHORD_KEYS, 9, FIG_INPUT_NO_LOOP };

int main(void)
{
    fig_engine_init(RESOLUTION_320x240);
    joypad_init();
    fig_input_init();

    const fm_vec3_t O = {{ 0, 0, 0 }};
    fig_prim_box(&g_body, O, (fm_vec3_t){{ 7, 9, 7 }},
                  fig_prim_rgba(0xFF, 0xE0, 0x50), fig_prim_rgba(0xE0, 0xA0, 0x18), fig_prim_rgba(0x60, 0x40, 0x00));
    fig_prim_box(&g_nose, (fm_vec3_t){{ 0, 3, 9 }}, (fm_vec3_t){{ 3, 3, 3 }}, 0xFFFFFFFF, 0xE0E0E8FF, 0x808080FF);
    fig_prim_box(&g_enemy, O, (fm_vec3_t){{ 9, 8, 9 }},
                  fig_prim_rgba(0xFF, 0x60, 0x80), fig_prim_rgba(0xC8, 0x38, 0x58), fig_prim_rgba(0x50, 0x10, 0x20));
    fig_prim_box(&g_enemy_eye, (fm_vec3_t){{ 0, 2, 9 }}, (fm_vec3_t){{ 5, 2, 1 }},
                  fig_prim_rgba(0xFF, 0xF0, 0x60), fig_prim_rgba(0xFF, 0xF0, 0x60), fig_prim_rgba(0xFF, 0xF0, 0x60));
    FigPrim floor_prim;
    fig_prim_floor(&floor_prim, 140.0f, 14, fig_prim_rgba(0x2E, 0x34, 0x40), fig_prim_rgba(0x26, 0x2C, 0x36));
    FigTransform floor_xf;
    fig_transform_init(&floor_xf);

    fig_actor_system_init(PROFILES, PROFILE_COUNT, g_pool, ACTOR_POOL_CAP);
    fig_actor_spawn(PROFILE_PLAYER, (fm_vec3_t){{ 0, 9, -30 }}, 0.0f, NULL);
    spawn_enemy(-50, 30, 26.0f, 0.9f, 0.0f);
    spawn_enemy( 55, 50, 34.0f, 1.3f, 2.0f);

    fig_prof_init();
#ifdef KILN_DEBUG
    fig_console_init();
    fig_console_register(CMDS, sizeof CMDS / sizeof CMDS[0]);
    fig_console_log("debug-demo ready");
    fig_console_log("open: hold Start + C-Up Left Down Right");
    fig_console_log("try: help  actors  spawn  overlay  prof");
#endif

    if (KILN_JUMP == JUMP_CONSOLE) fig_input_play(1, &CHORD);
    else fig_input_set_attract(1, &WANDER, 120);

    FigScene scene;
    fig_scene_init(&scene);
    fig_prim_stage(&scene, RGBA32(0x14, 0x16, 0x20, 0xFF), 220.0f, 420.0f);
    scene.fov_deg = 60.0f;
    scene.near_z = 10.0f;
    scene.far_z = 420.0f;

    uint32_t frames = 0;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        const float dt = 1.0f / 60.0f;
        FIG_PROF_BEGIN(FIG_PROF_UPDATE);

        fig_input_update();
#ifdef KILN_DEBUG
        fig_console_update(1);
#endif
        if (fig_input_pressed(1, FIG_BTN_Z)) g_overlay = !g_overlay;

        fig_actor_update_all(dt);

        FigActor *player = fig_actor_first(FIG_ACTOR_CAT_PLAYER);
        if (player) {
            const fm_vec3_t pp = player->xform.pos;
            scene.cam_target = (fm_vec3_t){{ pp.v[0] * 0.6f, 8, pp.v[2] * 0.6f + 30 }};
            scene.cam_pos = (fm_vec3_t){{ pp.v[0] * 0.5f, 120, pp.v[2] * 0.5f - 130 }};
        }
        fig_scene_update(&scene);

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            g_dbg_fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        FIG_PROF_END(FIG_PROF_UPDATE);
        fig_prof_frame_done();

        fig_frame_begin();
        FIG_PROF_BEGIN(FIG_PROF_SCENE);
        fig_scene_begin(&scene);
        fig_transform_push(&floor_xf); fig_prim_draw(&floor_prim); fig_transform_pop();
        fig_actor_draw_all();
        FIG_PROF_END(FIG_PROF_SCENE);

        fig_gui_begin();
        FIG_PROF_BEGIN(FIG_PROF_GUI);

        FIG_PROF_BEGIN(FIG_PROF_DEBUG);
        if (g_overlay) {
            fig_dd_begin(&scene, SCREEN_W, SCREEN_H);
            fig_dd_axes((fm_vec3_t){{ 0, 0.5f, 0 }}, 40.0f);
            for (FigActor *e = fig_actor_first(FIG_ACTOR_CAT_ENEMY); e; e = fig_actor_next(e)) {
                const EnemyState *s = (const EnemyState *)e->state;
                fm_vec3_t ring[ORBIT_POINTS + 1];
                for (int i = 0; i <= ORBIT_POINTS; i++) {
                    const float a = 6.28318f * (float)i / ORBIT_POINTS;
                    ring[i] = (fm_vec3_t){{ s->centre.v[0] + fm_cosf(a) * s->radius, 1,
                                            s->centre.v[2] + fm_sinf(a) * s->radius }};
                }
                fig_dd_path(ring, ORBIT_POINTS + 1, RGBA32(0x60, 0x80, 0xC0, 0xFF));
                fig_dd_box(e->xform.pos, (fm_vec3_t){{ 9, 8, 9 }}, RGBA32(0xFF, 0x80, 0xA0, 0xFF));
                fig_dd_text((fm_vec3_t){{ e->xform.pos.v[0], 24, e->xform.pos.v[2] }},
                             RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "enemy");
            }
            if (player) {
                fig_dd_box(player->xform.pos, (fm_vec3_t){{ 7, 9, 7 }}, RGBA32(0xFF, 0xE0, 0x60, 0xFF));
                fig_dd_text((fm_vec3_t){{ player->xform.pos.v[0], 24, player->xform.pos.v[2] }},
                             RGBA32(0xFF, 0xE0, 0x60, 0xFF), "%.0f %.0f", player->xform.pos.v[0], player->xform.pos.v[2]);
            }
            fig_dd_end();
        }

        /* Profiler: one bar per zone, scaled against a 16.7 ms frame. */
        const color_t teal = RGBA32(0x00, 0xF5, 0xD4, 0xFF);
        fig_gui_panel(8, 8, 136, 92, RGBA32(0x0C, 0x10, 0x1C, 0xFF), teal);
        fig_gui_text(14, 21, teal, "KILN DEBUG");
        fig_gui_text(80, 21, RGBA32(0x90, 0x98, 0xB0, 0xFF), "%4.1f fps", g_dbg_fps);
        for (int z = 0; z < FIG_PROF_TOTAL; z++) {
            const int y = 34 + z * 12;
            const float ms = fig_prof_ms(z);
            int w = (int)(ms / 16.7f * 60.0f);
            if (w > 60) w = 60;
            fig_gui_text(14, y, RGBA32(0xE8, 0xE8, 0xF0, 0xFF), "%-6s", fig_prof_label(z));
            fig_gui_rect(56, y - 6, 60, 5, RGBA32(0x30, 0x34, 0x44, 0xFF));
            fig_gui_rect(56, y - 6, w, 5, ms > 8.0f ? RGBA32(0xFF, 0x60, 0x40, 0xFF) : teal);
            fig_gui_text(120, y, RGBA32(0x90, 0x98, 0xB0, 0xFF), "%.1f", ms);
        }

        /* Bottom-right, above the hint line: fig_console's closed log panel
         * owns the top-right corner and covered the badge on console. */
        if (fig_input_scripted(1)) {
            fig_gui_panel(SCREEN_W - 58, SCREEN_H - 34, 50, 16, RGBA32(0xC0, 0x30, 0x60, 0xFF), RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
            fig_gui_text(SCREEN_W - 49, SCREEN_H - 22, RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "DEMO");
        }
        fig_gui_text(8, SCREEN_H - 6, RGBA32(0x8B, 0x5C, 0xF6, 0xFF), "Start + C-U C-L C-D C-R: console   Z: overlay");

#ifdef KILN_DEBUG
        fig_console_draw();
#endif
        FIG_PROF_END(FIG_PROF_DEBUG);
        FIG_PROF_END(FIG_PROF_GUI);
        fig_gui_end();
        fig_frame_end();
    }
}
