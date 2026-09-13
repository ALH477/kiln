// SPDX-License-Identifier: MIT
//
// debug-demo: the engine's debugging kit, all on screen at once.
//
//   kiln_console    the retro on-screen console. Hold Start and press the C
//                   buttons counter-clockwise (C-Up, C-Left, C-Down, C-Right)
//                   to open it; the stick moves the keyboard cursor, A types,
//                   B backspaces, Start submits. Commands: help, clear, exit,
//                   plus actors, fps, spawn, overlay, prof and panic.
//   kiln_debugdraw  world-space overlays drawn in the 2D pass: each enemy's
//                   bounds and orbit, the world axes, labels — the spatial
//                   state that is otherwise only numbers in a HUD
//   kiln_prof       per-zone CPU time, drawn as bars
//   kiln_panic      `panic` dereferences NULL, to show the crash screen
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

static KilnPrim g_body, g_nose, g_enemy, g_enemy_eye;
static int g_overlay = 1;
static float g_dbg_fps = 60.0f;

// ── Player ──────────────────────────────────────────────────────────────
static void player_init(KilnActor *self, const KilnDict *args)
{
    (void)args;
    self->xform.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
}

static void player_update(KilnActor *self, float dt)
{
    /* While the console is open the stick drives its keyboard, not the player. */
#ifdef KILN_DEBUG
    if (kiln_console_is_open()) return;
#endif
    const KilnInput *in = kiln_input_get(1);
    const float dx = -in->stick_x * 80.0f * dt, dz = in->stick_y * 80.0f * dt;
    self->xform.pos.v[0] += dx;
    self->xform.pos.v[2] += dz;
    if (dx * dx + dz * dz > 1e-4f) self->xform.rot_angle = fm_atan2f(dx, dz);
}

static void player_draw(KilnActor *self) { (void)self; kiln_prim_draw(&g_body); kiln_prim_draw(&g_nose); }

// ── Enemy: orbits the point it spawned at ───────────────────────────────
typedef struct { float angle, radius, speed; fm_vec3_t centre; } EnemyState;

static void enemy_init(KilnActor *self, const KilnDict *args)
{
    (void)args;
    EnemyState *s = (EnemyState *)self->state;
    *s = (EnemyState){ .radius = 26.0f, .speed = 1.0f, .centre = self->xform.pos };
    self->xform.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
}

static void enemy_update(KilnActor *self, float dt)
{
    EnemyState *s = (EnemyState *)self->state;
    s->angle += s->speed * dt;
    /* Around its own spawn point. The old demo orbited the world origin, so
     * both enemies drifted out of frame as soon as the player walked away. */
    self->xform.pos.v[0] = s->centre.v[0] + fm_cosf(s->angle) * s->radius;
    self->xform.pos.v[2] = s->centre.v[2] + fm_sinf(s->angle) * s->radius;
    self->xform.rot_angle = -s->angle;
}

static void enemy_draw(KilnActor *self) { (void)self; kiln_prim_draw(&g_enemy); kiln_prim_draw(&g_enemy_eye); }

static const KilnActorProfile PROFILES[PROFILE_COUNT] = {
    [PROFILE_PLAYER] = { .name = "player", .category = KILN_ACTOR_CAT_PLAYER,
                         .init = player_init, .update = player_update, .draw = player_draw },
    [PROFILE_ENEMY]  = { .name = "enemy", .category = KILN_ACTOR_CAT_ENEMY,
                         .state_size = sizeof(EnemyState),
                         .init = enemy_init, .update = enemy_update, .draw = enemy_draw },
};

static KilnActor g_pool[ACTOR_POOL_CAP];

static void spawn_enemy(float x, float z, float radius, float speed, float phase)
{
    KilnActor *a = kiln_actor_resolve(kiln_actor_spawn(PROFILE_ENEMY, (fm_vec3_t){{ x, 10, z }}, 0, NULL));
    if (!a) return;
    EnemyState *s = (EnemyState *)a->state;
    s->radius = radius; s->speed = speed; s->angle = phase;
}

// ── Console commands ────────────────────────────────────────────────────
#ifdef KILN_DEBUG
static void cmd_actors(int argc, const char **argv)
{
    (void)argc; (void)argv;
    kiln_console_log("player %u enemy %u total %u", kiln_actor_count(KILN_ACTOR_CAT_PLAYER),
                     kiln_actor_count(KILN_ACTOR_CAT_ENEMY), kiln_actor_count(KILN_ACTOR_CATEGORY_COUNT));
}

static void cmd_fps(int argc, const char **argv)
{
    (void)argc; (void)argv;
    kiln_console_log("fps %.1f  frame %.2f ms", g_dbg_fps, kiln_prof_ms(KILN_PROF_TOTAL));
}

static void cmd_spawn(int argc, const char **argv)
{
    (void)argc; (void)argv;
    KilnActor *p = kiln_actor_first(KILN_ACTOR_CAT_PLAYER);
    const float x = p ? p->xform.pos.v[0] : 0, z = p ? p->xform.pos.v[2] + 50 : 50;
    spawn_enemy(x, z, 18.0f, 1.6f, 0.0f);
    kiln_console_log("spawned an enemy at %.0f %.0f", x, z);
}

static void cmd_overlay(int argc, const char **argv)
{
    (void)argc; (void)argv;
    g_overlay = !g_overlay;
    kiln_console_log("overlay %s", g_overlay ? "on" : "off");
}

static void cmd_prof(int argc, const char **argv)
{
    (void)argc; (void)argv;
    kiln_prof_print();
}

static void cmd_panic(int argc, const char **argv)
{
    (void)argc; (void)argv;
    kiln_panic_message("user triggered panic via console\n");
    *(volatile int *)0 = 0; /* deliberate null dereference */
}

static const KilnConsoleCmd CMDS[] = {
    { "actors",  "count actors by category",    cmd_actors },
    { "fps",     "print frame rate and time",   cmd_fps },
    { "spawn",   "spawn an enemy ahead of you", cmd_spawn },
    { "overlay", "toggle the debugdraw layer",  cmd_overlay },
    { "prof",    "log every profiler zone",     cmd_prof },
    { "panic",   "trigger a deliberate crash",  cmd_panic },
};
#endif

// ── Tapes ───────────────────────────────────────────────────────────────
static const KilnInputKey WANDER_KEYS[] = {
    { .frame =   0, .sy =  80 },
    { .frame =  60, .sx = -70, .sy = 30 },
    { .frame = 130, .sy = -80 },
    { .frame = 200, .sx =  70, .sy = -20 },
    { .frame = 270, .sx =  30, .sy =  70 },
    { .frame = 330 },
    { .frame = 360 },
};
static const KilnInputTape WANDER = { WANDER_KEYS, 7, 0 };

/* Start held throughout; C-Up, C-Left, C-Down, C-Right pressed in turn. */
static const KilnInputKey CHORD_KEYS[] = {
    { .frame =  0, .buttons = KILN_BTN_START },
    { .frame = 10, .buttons = KILN_BTN_START | KILN_BTN_CU },
    { .frame = 14, .buttons = KILN_BTN_START },
    { .frame = 18, .buttons = KILN_BTN_START | KILN_BTN_CL },
    { .frame = 22, .buttons = KILN_BTN_START },
    { .frame = 26, .buttons = KILN_BTN_START | KILN_BTN_CD },
    { .frame = 30, .buttons = KILN_BTN_START },
    { .frame = 34, .buttons = KILN_BTN_START | KILN_BTN_CR },
    { .frame = 38 },
};
static const KilnInputTape CHORD = { CHORD_KEYS, 9, KILN_INPUT_NO_LOOP };

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    kiln_input_init();

    const fm_vec3_t O = {{ 0, 0, 0 }};
    kiln_prim_box(&g_body, O, (fm_vec3_t){{ 7, 9, 7 }},
                  kiln_prim_rgba(0xFF, 0xE0, 0x50), kiln_prim_rgba(0xE0, 0xA0, 0x18), kiln_prim_rgba(0x60, 0x40, 0x00));
    kiln_prim_box(&g_nose, (fm_vec3_t){{ 0, 3, 9 }}, (fm_vec3_t){{ 3, 3, 3 }}, 0xFFFFFFFF, 0xE0E0E8FF, 0x808080FF);
    kiln_prim_box(&g_enemy, O, (fm_vec3_t){{ 9, 8, 9 }},
                  kiln_prim_rgba(0xFF, 0x60, 0x80), kiln_prim_rgba(0xC8, 0x38, 0x58), kiln_prim_rgba(0x50, 0x10, 0x20));
    kiln_prim_box(&g_enemy_eye, (fm_vec3_t){{ 0, 2, 9 }}, (fm_vec3_t){{ 5, 2, 1 }},
                  kiln_prim_rgba(0xFF, 0xF0, 0x60), kiln_prim_rgba(0xFF, 0xF0, 0x60), kiln_prim_rgba(0xFF, 0xF0, 0x60));
    KilnPrim floor_prim;
    kiln_prim_floor(&floor_prim, 140.0f, 14, kiln_prim_rgba(0x2E, 0x34, 0x40), kiln_prim_rgba(0x26, 0x2C, 0x36));
    KilnTransform floor_xf;
    kiln_transform_init(&floor_xf);

    kiln_actor_system_init(PROFILES, PROFILE_COUNT, g_pool, ACTOR_POOL_CAP);
    kiln_actor_spawn(PROFILE_PLAYER, (fm_vec3_t){{ 0, 9, -30 }}, 0.0f, NULL);
    spawn_enemy(-50, 30, 26.0f, 0.9f, 0.0f);
    spawn_enemy( 55, 50, 34.0f, 1.3f, 2.0f);

    kiln_prof_init();
#ifdef KILN_DEBUG
    kiln_console_init();
    kiln_console_register(CMDS, sizeof CMDS / sizeof CMDS[0]);
    kiln_console_log("debug-demo ready");
    kiln_console_log("open: hold Start + C-Up Left Down Right");
    kiln_console_log("try: help  actors  spawn  overlay  prof");
#endif

    if (KILN_JUMP == JUMP_CONSOLE) kiln_input_play(1, &CHORD);
    else kiln_input_set_attract(1, &WANDER, 120);

    KilnScene scene;
    kiln_scene_init(&scene);
    kiln_prim_stage(&scene, RGBA32(0x14, 0x16, 0x20, 0xFF), 220.0f, 420.0f);
    scene.fov_deg = 60.0f;
    scene.near_z = 10.0f;
    scene.far_z = 420.0f;

    uint32_t frames = 0;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        const float dt = 1.0f / 60.0f;
        KILN_PROF_BEGIN(KILN_PROF_UPDATE);

        kiln_input_update();
#ifdef KILN_DEBUG
        kiln_console_update(1);
#endif
        if (kiln_input_pressed(1, KILN_BTN_Z)) g_overlay = !g_overlay;

        kiln_actor_update_all(dt);

        KilnActor *player = kiln_actor_first(KILN_ACTOR_CAT_PLAYER);
        if (player) {
            const fm_vec3_t pp = player->xform.pos;
            scene.cam_target = (fm_vec3_t){{ pp.v[0] * 0.6f, 8, pp.v[2] * 0.6f + 30 }};
            scene.cam_pos = (fm_vec3_t){{ pp.v[0] * 0.5f, 120, pp.v[2] * 0.5f - 130 }};
        }
        kiln_scene_update(&scene);

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            g_dbg_fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        KILN_PROF_END(KILN_PROF_UPDATE);
        kiln_prof_frame_done();

        kiln_frame_begin();
        KILN_PROF_BEGIN(KILN_PROF_SCENE);
        kiln_scene_begin(&scene);
        kiln_transform_push(&floor_xf); kiln_prim_draw(&floor_prim); kiln_transform_pop();
        kiln_actor_draw_all();
        KILN_PROF_END(KILN_PROF_SCENE);

        kiln_gui_begin();
        KILN_PROF_BEGIN(KILN_PROF_GUI);

        KILN_PROF_BEGIN(KILN_PROF_DEBUG);
        if (g_overlay) {
            kiln_dd_begin(&scene, SCREEN_W, SCREEN_H);
            kiln_dd_axes((fm_vec3_t){{ 0, 0.5f, 0 }}, 40.0f);
            for (KilnActor *e = kiln_actor_first(KILN_ACTOR_CAT_ENEMY); e; e = kiln_actor_next(e)) {
                const EnemyState *s = (const EnemyState *)e->state;
                fm_vec3_t ring[ORBIT_POINTS + 1];
                for (int i = 0; i <= ORBIT_POINTS; i++) {
                    const float a = 6.28318f * (float)i / ORBIT_POINTS;
                    ring[i] = (fm_vec3_t){{ s->centre.v[0] + fm_cosf(a) * s->radius, 1,
                                            s->centre.v[2] + fm_sinf(a) * s->radius }};
                }
                kiln_dd_path(ring, ORBIT_POINTS + 1, RGBA32(0x60, 0x80, 0xC0, 0xFF));
                kiln_dd_box(e->xform.pos, (fm_vec3_t){{ 9, 8, 9 }}, RGBA32(0xFF, 0x80, 0xA0, 0xFF));
                kiln_dd_text((fm_vec3_t){{ e->xform.pos.v[0], 24, e->xform.pos.v[2] }},
                             RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "enemy");
            }
            if (player) {
                kiln_dd_box(player->xform.pos, (fm_vec3_t){{ 7, 9, 7 }}, RGBA32(0xFF, 0xE0, 0x60, 0xFF));
                kiln_dd_text((fm_vec3_t){{ player->xform.pos.v[0], 24, player->xform.pos.v[2] }},
                             RGBA32(0xFF, 0xE0, 0x60, 0xFF), "%.0f %.0f", player->xform.pos.v[0], player->xform.pos.v[2]);
            }
            kiln_dd_end();
        }

        /* Profiler: one bar per zone, scaled against a 16.7 ms frame. */
        const color_t teal = RGBA32(0x00, 0xF5, 0xD4, 0xFF);
        kiln_gui_panel(8, 8, 136, 92, RGBA32(0x0C, 0x10, 0x1C, 0xFF), teal);
        kiln_gui_text(14, 21, teal, "KILN DEBUG");
        kiln_gui_text(80, 21, RGBA32(0x90, 0x98, 0xB0, 0xFF), "%4.1f fps", g_dbg_fps);
        for (int z = 0; z < KILN_PROF_TOTAL; z++) {
            const int y = 34 + z * 12;
            const float ms = kiln_prof_ms(z);
            int w = (int)(ms / 16.7f * 60.0f);
            if (w > 60) w = 60;
            kiln_gui_text(14, y, RGBA32(0xE8, 0xE8, 0xF0, 0xFF), "%-6s", kiln_prof_label(z));
            kiln_gui_rect(56, y - 6, 60, 5, RGBA32(0x30, 0x34, 0x44, 0xFF));
            kiln_gui_rect(56, y - 6, w, 5, ms > 8.0f ? RGBA32(0xFF, 0x60, 0x40, 0xFF) : teal);
            kiln_gui_text(120, y, RGBA32(0x90, 0x98, 0xB0, 0xFF), "%.1f", ms);
        }

        if (kiln_input_scripted(1)) {
            kiln_gui_panel(SCREEN_W - 58, 8, 50, 16, RGBA32(0xC0, 0x30, 0x60, 0xFF), RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
            kiln_gui_text(SCREEN_W - 49, 20, RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "DEMO");
        }
        kiln_gui_text(8, SCREEN_H - 6, RGBA32(0x8B, 0x5C, 0xF6, 0xFF), "Start + C-U C-L C-D C-R: console   Z: overlay");

#ifdef KILN_DEBUG
        kiln_console_draw();
#endif
        KILN_PROF_END(KILN_PROF_DEBUG);
        KILN_PROF_END(KILN_PROF_GUI);
        kiln_gui_end();
        kiln_frame_end();
    }
}
