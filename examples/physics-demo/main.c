// SPDX-License-Identifier: MIT
//
// fig_physics + fig_room brush auto-install + fig_clip broadphase, in one room.
//
//   fig_room     -> one room; on_load fills room->brushes (floor + 4 walls) and
//                   fig_room installs them into the clip world — this ROM never
//                   calls fig_clip_set_world
//   fig_physics  -> a pyramid of 14 crates that settle, stack and sleep; A punts
//                   the nearest one in front of you, which wakes whatever it hits
//   fig_clip     -> the player slides on the same brushes; the broadphase grid
//                   is a runtime toggle, and the HUD's trace count shows its win
//
// Crates dim when fig_physics puts them to sleep, so "is the simulation still
// running" is visible rather than a number.
//
//   stick  move          A      punt           START  restack
//   D <    physics on/off D >   broadphase     Z      body boxes
//   idle 2 s: the demo walks up to the stack and punts it, and restacks after
//
// Jumps: .#physics-demo-punt walks up and punts once, then leaves the pile to
// settle; .#physics-demo-bp boots with the broadphase and the box overlay on.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_clip.h>
#include <kiln/kiln_actor.h>
#include <kiln/kiln_room.h>
#include <kiln/kiln_physics.h>
#include <kiln/kiln_prim.h>
#include <kiln/kiln_debugdraw.h>

enum { JUMP_NONE, JUMP_PUNT, JUMP_BP };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W 320
#define SCREEN_H 240
#define ACTOR_POOL_CAP 4
#define BODY_CAP 16
#define CRATE_HALF 10.0f
#define ROOM_HALF 120.0f
#define WALL_H 36.0f
#define WALL_T 6.0f
#define FLOOR_TOP 1.0f
#define WALK_SPEED 100.0f
#define PUNT_RANGE 110.0f

/* Player box relative to its centre: mins NEGATIVE. This demo used to pass the
 * half-extent as both, which shifted the box a full half-extent on every axis
 * and sank it into the floor brush — so fig_clip ignored the floor it started
 * inside. */
static const fm_vec3_t P_MINS = {{ -8, -10, -8 }};
static const fm_vec3_t P_MAXS = {{  8,  10,  8 }};

static const uint32_t CRATE_COLOURS[6] = {
    0xF2A03CFF, 0xE8604CFF, 0x5CC8A0FF, 0x6C9CF0FF, 0xD878C8FF, 0xE8D860FF,
};

// ── Tapes ───────────────────────────────────────────────────────────────
// Camera looks down +Z, so stick up walks toward the stack.
static const FigInputKey ATTRACT_KEYS[] = {
    { .frame =   0, .sy =  85 },
    { .frame =  14 },
    { .frame =  30, .buttons = FIG_BTN_A },
    { .frame =  66 },
    { .frame = 150, .sy = -85 },
    { .frame = 190, .sx = 80, .sy = 40 },
    { .frame = 250, .sx = -60, .sy = 60 },
    { .frame = 300, .buttons = FIG_BTN_A },
    { .frame = 306, .sx = 60, .sy = -30 },
    { .frame = 380, .sy = -70 },
    { .frame = 440 },
};
static const FigInputTape ATTRACT = { ATTRACT_KEYS, 11, 0 };

static const FigInputKey PUNT_KEYS[] = {
    { .frame =   0 },
    { .frame =  30, .sy = 85 },
    { .frame =  44 },
    { .frame =  90, .buttons = FIG_BTN_A },
    { .frame =  96 },
};
static const FigInputTape PUNT = { PUNT_KEYS, 5, FIG_INPUT_NO_LOOP };

// ── Player actor ────────────────────────────────────────────────────────
typedef struct { float yaw; } PlayerState;

static FigPrim g_body, g_nose, g_shadow, g_floor;
/* One mesh per crate colour, awake and asleep. Prebuilt rather than recoloured
 * per crate per frame: the RSP reads a vertex buffer asynchronously after
 * t3d_vert_load, so rewriting one shared buffer between crates would hand an
 * earlier crate the next one's colours on console, and no host render shows it. */
static FigPrim g_crate[6][2];
static FigTransform g_shadow_xf;

static void player_init(FigActor *self, const FigDict *args)
{
    (void)args;
    ((PlayerState *)self->state)->yaw = 0.0f;
    self->xform.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
}

static void player_update(FigActor *self, float dt)
{
    PlayerState *s = (PlayerState *)self->state;
    const FigInput *in = fig_input_get(1);

    /* Camera-relative: up is +Z, right is -X for a camera looking down +Z. */
    const fm_vec3_t disp = {{ -in->stick_x * WALK_SPEED * dt, 0,
                               in->stick_y * WALK_SPEED * dt }};
    self->xform.pos = fig_clip_slide(self->xform.pos, disp, P_MINS, P_MAXS, 4);

    /* Facing follows the last direction actually pushed. The old test was
     * `dx*dx + dz*dz > 1`, and a frame's move never exceeds 0.3 units, so the
     * yaw never updated and every punt went toward +X. */
    if (disp.v[0] * disp.v[0] + disp.v[2] * disp.v[2] > 1e-4f)
        s->yaw = fm_atan2f(disp.v[0], disp.v[2]);
    self->xform.rot_angle = s->yaw;
}

static void player_draw(FigActor *self)
{
    (void)self;
    fig_prim_draw(&g_body);
    fig_prim_draw(&g_nose);
}

static const FigActorProfile PROFILES[1] = {
    { .name = "player", .category = FIG_ACTOR_CAT_PLAYER,
      .state_size = sizeof(PlayerState),
      .init = player_init, .update = player_update, .draw = player_draw },
};
enum { PROFILE_PLAYER = 0, PROFILE_COUNT = 1 };

static FigActor g_pool[ACTOR_POOL_CAP];

// ── Room: floor + 4 walls, as brushes and as prims from the same AABBs ────
static FigBrush g_room_brushes[5];
static FigPrim g_wall_prim[4];

static fm_vec3_t centre_of(const FigBrush *b)
{
    return (fm_vec3_t){{ (b->mins.v[0] + b->maxs.v[0]) * 0.5f, (b->mins.v[1] + b->maxs.v[1]) * 0.5f,
                         (b->mins.v[2] + b->maxs.v[2]) * 0.5f }};
}
static fm_vec3_t half_of(const FigBrush *b)
{
    return (fm_vec3_t){{ (b->maxs.v[0] - b->mins.v[0]) * 0.5f, (b->maxs.v[1] - b->mins.v[1]) * 0.5f,
                         (b->maxs.v[2] - b->mins.v[2]) * 0.5f }};
}

static void room_load(FigRoom *room, void *user)
{
    (void)user;
    const fm_vec3_t mn = room->aabb_min, mx = room->aabb_max;
    g_room_brushes[0] = (FigBrush){ .mins = {{ mn.v[0], mn.v[1] - 8, mn.v[2] }},
                                     .maxs = {{ mx.v[0], FLOOR_TOP, mx.v[2] }} };
    g_room_brushes[1] = (FigBrush){ .mins = {{ mn.v[0], 0, mn.v[2] }},
                                     .maxs = {{ mn.v[0] + WALL_T, WALL_H, mx.v[2] }} };
    g_room_brushes[2] = (FigBrush){ .mins = {{ mx.v[0] - WALL_T, 0, mn.v[2] }},
                                     .maxs = {{ mx.v[0], WALL_H, mx.v[2] }} };
    g_room_brushes[3] = (FigBrush){ .mins = {{ mn.v[0], 0, mn.v[2] }},
                                     .maxs = {{ mx.v[0], WALL_H, mn.v[2] + WALL_T }} };
    g_room_brushes[4] = (FigBrush){ .mins = {{ mn.v[0], 0, mx.v[2] - WALL_T }},
                                     .maxs = {{ mx.v[0], WALL_H, mx.v[2] }} };
    room->brushes = g_room_brushes;
    room->brush_count = 5;

    for (int i = 0; i < 4; i++)
        fig_prim_box(&g_wall_prim[i], centre_of(&g_room_brushes[i + 1]), half_of(&g_room_brushes[i + 1]),
                      fig_prim_rgba(0xB8, 0xC0, 0xD0), fig_prim_rgba(0x68, 0x74, 0x90),
                      fig_prim_rgba(0x20, 0x20, 0x28));
    fig_prim_floor(&g_floor, ROOM_HALF, 12, fig_prim_rgba(0x4C, 0x58, 0x6C),
                    fig_prim_rgba(0x40, 0x4A, 0x5C));
}

static void room_unload(FigRoom *room, void *user)
{
    (void)user;
    for (int i = 0; i < 4; i++) fig_prim_free(&g_wall_prim[i]);
    fig_prim_free(&g_floor);
    room->brushes = NULL;
    room->brush_count = 0;
}

static void room_spawn(FigRoom *room, const FigRoomSpawn *spawn, void *user)
{
    (void)room; (void)spawn; (void)user;
}

static void room_draw(FigRoom *room, void *user)
{
    (void)room; (void)user;
    static FigTransform floor_xf;
    static int floor_xf_ready;
    if (!floor_xf_ready) { fig_transform_init(&floor_xf); floor_xf_ready = 1; }
    floor_xf.pos = (fm_vec3_t){{ 0, FLOOR_TOP, 0 }};
    fig_transform_push(&floor_xf);
    fig_prim_draw(&g_floor);
    fig_transform_pop();
    for (int i = 0; i < 4; i++) fig_prim_draw(&g_wall_prim[i]);
}

static FigRoomSystem g_sys;
static FigRoom g_room = {
    .id = 0,
    .aabb_min = {{ -ROOM_HALF, 0, -ROOM_HALF }},
    .aabb_max = {{  ROOM_HALF, 0,  ROOM_HALF }},
    .neighbour_count = 0,
    .spawn_count = 0,
};

// ── Physics world ───────────────────────────────────────────────────────
static FigPhysicsBody g_bodies[BODY_CAP];
static FigPhysicsWorld g_phys;
static FigTransform g_crate_xf[BODY_CAP];

/* 3x3, then 2x2, then 1: fourteen crates, each resting on the layer below. */
static void stack_crates(void)
{
    fig_physics_init(&g_phys, g_bodies, BODY_CAP);
    const float step = CRATE_HALF * 2.0f + 0.5f;
    const fm_vec3_t half = {{ CRATE_HALF, CRATE_HALF, CRATE_HALF }};
    for (int layer = 0; layer < 3; layer++) {
        const int n = 3 - layer;
        const float y = FLOOR_TOP + CRATE_HALF + 0.5f + layer * step;
        for (int j = 0; j < n; j++) {
            for (int i = 0; i < n; i++) {
                fm_vec3_t p = {{ (i - (n - 1) * 0.5f) * step, y,
                                 40.0f + (j - (n - 1) * 0.5f) * step }};
                fig_physics_spawn(&g_phys, FIG_PHYS_DYNAMIC, p, half, 5.0f);
            }
        }
    }
}

int main(void)
{
    fig_engine_init(RESOLUTION_320x240);
    joypad_init();
    fig_input_init();

    fig_prim_box(&g_body, (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 8, 10, 8 }},
                  fig_prim_rgba(0xFF, 0xE0, 0x50), fig_prim_rgba(0xE0, 0xA0, 0x18),
                  fig_prim_rgba(0x60, 0x40, 0x00));
    fig_prim_box(&g_nose, (fm_vec3_t){{ 0, 3, 10 }}, (fm_vec3_t){{ 3, 3, 3 }},
                  fig_prim_rgba(0xFF, 0xFF, 0xFF), fig_prim_rgba(0xE0, 0xE0, 0xE8),
                  fig_prim_rgba(0x80, 0x80, 0x80));
    fig_prim_floor(&g_shadow, 11.0f, 1, fig_prim_rgba(0x14, 0x18, 0x20),
                    fig_prim_rgba(0x14, 0x18, 0x20));
    for (int c = 0; c < 6; c++) {
        for (int asleep = 0; asleep < 2; asleep++) {
            const uint32_t top = asleep ? fig_prim_shade(CRATE_COLOURS[c], 0.55f) : CRATE_COLOURS[c];
            fig_prim_box(&g_crate[c][asleep], (fm_vec3_t){{ 0, 0, 0 }},
                          (fm_vec3_t){{ CRATE_HALF, CRATE_HALF, CRATE_HALF }},
                          top, fig_prim_shade(top, 0.72f), fig_prim_shade(top, 0.3f));
        }
    }
    fig_transform_init(&g_shadow_xf);
    for (int i = 0; i < BODY_CAP; i++) fig_transform_init(&g_crate_xf[i]);

    fig_actor_system_init(PROFILES, PROFILE_COUNT, g_pool, ACTOR_POOL_CAP);
    fig_room_system_init(&g_sys, &g_room, 1, 1,
                          room_load, room_unload, room_spawn, room_draw, NULL,
                          /*owns_clip_world=*/1);
    stack_crates();

    /* On the floor: floor top + half height + a hair, so the player does not
     * begin inside the floor brush. */
    FigActorHandle player_h = fig_actor_spawn(PROFILE_PLAYER,
        (fm_vec3_t){{ 0, FLOOR_TOP + 10.5f, -30 }}, 0.0f, NULL);

    FigScene scene;
    fig_scene_init(&scene);
    fig_prim_stage(&scene, RGBA32(0x24, 0x22, 0x34, 0xFF), 260.0f, 560.0f);
    scene.fov_deg = 62.0f;
    scene.near_z = 12.0f;
    scene.far_z = 560.0f;

    int phys_on = 1, bp_on = (KILN_JUMP == JUMP_BP), show_boxes = (KILN_JUMP == JUMP_BP);
    fig_clip_set_broadphase(bp_on);
    if (KILN_JUMP == JUMP_PUNT) fig_input_play(1, &PUNT);
    else fig_input_set_attract(1, &ATTRACT, 120);

    uint16_t last_trace = 0;
    int flash = 0;
    fm_vec3_t flash_to = {{ 0, 0, 0 }};
    float calm = 0.0f;
    int punts = 0;
    uint32_t frames = 0;
    float fps = 60.0f;
    uint32_t last_ticks = get_ticks();
    fm_vec3_t cam_target = {{ 0, 0, 5 }};

    for (;;) {
        fig_input_update();
        const FigInput *in = fig_input_get(1);
        const float dt = 1.0f / 60.0f;

        if (in->edges & FIG_BTN_DL) { phys_on = !phys_on; fig_physics_set_enabled(&g_phys, phys_on); }
        if (in->edges & FIG_BTN_DR) { bp_on = !bp_on; fig_clip_set_broadphase(bp_on); }
        if (in->edges & FIG_BTN_Z) show_boxes = !show_boxes;
        if (in->edges & FIG_BTN_START) { stack_crates(); calm = 0; }

        fig_actor_update_all(dt);
        FigActor *player = fig_actor_resolve(player_h);

        /* Punt: the nearest crate inside a ±60° cone ahead of the player. */
        if (player && (in->edges & FIG_BTN_A)) {
            const fm_vec3_t pp = player->xform.pos;
            const float yaw = ((PlayerState *)player->state)->yaw;
            const fm_vec3_t pf = {{ fm_sinf(yaw), 0, fm_cosf(yaw) }};
            FigPhysicsBody *best = NULL;
            float best_d = PUNT_RANGE;
            for (uint16_t i = 0; i < g_phys.count; i++) {
                FigPhysicsBody *b = &g_phys.bodies[i];
                if (b->type != FIG_PHYS_DYNAMIC) continue;
                fm_vec3_t d = {{ b->pos.v[0] - pp.v[0], 0, b->pos.v[2] - pp.v[2] }};
                const float dist = fm_vec3_len(&d);
                if (dist > best_d || dist < 1e-3f) continue;
                if ((d.v[0] * pf.v[0] + d.v[2] * pf.v[2]) / dist < 0.5f) continue;
                best = b; best_d = dist;
            }
            if (best) {
                fig_physics_apply_impulse(best, (fm_vec3_t){{ pf.v[0] * 1400.0f, 900.0f,
                                                                pf.v[2] * 1400.0f }});
                flash = 12;
                flash_to = best->pos;
                punts++;
            }
            calm = 0.0f;
        }

        fig_physics_step(&g_phys, dt);

        uint16_t sleeping = 0;
        for (uint16_t i = 0; i < g_phys.count; i++) if (g_phys.bodies[i].sleeping) sleeping++;

        /* While a tape is playing, restack once the pile has been still for a
         * while — so an unattended demo keeps showing something falling. */
        calm = (sleeping == g_phys.count) ? calm + dt : 0.0f;
        if (KILN_JUMP != JUMP_PUNT && fig_input_scripted(1) && calm > 3.0f) {
            stack_crates();
            calm = 0.0f;
        }

        const fm_vec3_t ppos = player ? player->xform.pos : (fm_vec3_t){{ 0, 0, 0 }};
        fig_room_system_update(&g_sys, ppos);

        /* Camera: behind and above, easing after the player, kept inside the
         * room so the south wall never blocks the view. */
        cam_target.v[0] += (ppos.v[0] * 0.5f - cam_target.v[0]) * 0.06f;
        cam_target.v[2] += ((ppos.v[2] + 40.0f) * 0.5f - cam_target.v[2]) * 0.06f;
        /* Aimed between the player and the stack, low enough that the
         * pyramid stands in the middle of the frame, clear of the HUD panel. */
        scene.cam_target = (fm_vec3_t){{ cam_target.v[0], 22, cam_target.v[2] }};
        float cz = cam_target.v[2] - 115.0f;
        if (cz < -ROOM_HALF + WALL_T + 4) cz = -ROOM_HALF + WALL_T + 4;
        scene.cam_pos = (fm_vec3_t){{ cam_target.v[0] * 0.6f, 100, cz }};
        fig_scene_update(&scene);

        if (player) {
            fig_clip_ground(player->xform.pos, P_MINS, P_MAXS);
            last_trace = fig_clip_last_trace_brushes();
        }

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        /* ── 3D ───────────────────────────────────────────────────── */
        fig_frame_begin();
        fig_scene_begin(&scene);

        fig_room_draw_all(&g_sys);

        if (player) {
            g_shadow_xf.pos = (fm_vec3_t){{ ppos.v[0], FLOOR_TOP + 0.4f, ppos.v[2] }};
            fig_transform_push(&g_shadow_xf);
            fig_prim_draw(&g_shadow);
            fig_transform_pop();
        }

        for (uint16_t i = 0; i < g_phys.count; i++) {
            const FigPhysicsBody *b = &g_phys.bodies[i];
            g_crate_xf[i].pos = b->pos;
            fig_transform_push(&g_crate_xf[i]);
            fig_prim_draw(&g_crate[i % 6][b->sleeping ? 1 : 0]);
            fig_transform_pop();
        }

        fig_actor_draw_all();

        /* ── 2D ───────────────────────────────────────────────────── */
        fig_gui_begin();

        if (show_boxes || flash > 0) {
            fig_dd_begin(&scene, SCREEN_W, SCREEN_H);
            if (show_boxes) {
                for (uint16_t i = 0; i < g_phys.count; i++) {
                    const FigPhysicsBody *b = &g_phys.bodies[i];
                    if (b->sleeping) continue;
                    fig_dd_box(b->pos, b->maxs, RGBA32(0x80, 0xFF, 0xF0, 0xFF));
                }
            }
            if (flash > 0) {
                const fm_vec3_t from = {{ ppos.v[0], ppos.v[1] + 4, ppos.v[2] }};
                fig_dd_line(from, flash_to, RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
                fig_dd_point(flash_to, 2 + flash / 2, RGBA32(0xFF, 0xF0, 0x80, 0xFF));
                flash--;
            }
            fig_dd_end();
        }

        const color_t ink = RGBA32(0xE8, 0xE8, 0xF0, 0xFF);
        const color_t teal = RGBA32(0x00, 0xF5, 0xD4, 0xFF);
        const color_t dim = RGBA32(0x90, 0x98, 0xB0, 0xFF);
        fig_gui_panel(8, 8, 150, 80, RGBA32(0x0C, 0x10, 0x1C, 0xFF), teal);
        fig_gui_text(14, 21, teal, "KILN PHYSICS");
        fig_gui_text(14, 34, ink, "phys %s  bp %s", phys_on ? "on " : "off", bp_on ? "on" : "off");
        fig_gui_text(14, 46, ink, "bodies %u  asleep %u", g_phys.count, sleeping);
        fig_gui_text(14, 58, ink, "traced %u/5  punts %d", last_trace, punts);
        fig_gui_rect(14, 64, 120, 4, RGBA32(0x30, 0x34, 0x44, 0xFF));
        fig_gui_rect(14, 64, 120 * (g_phys.count - sleeping) / (g_phys.count ? g_phys.count : 1), 4,
                      RGBA32(0xFF, 0xA0, 0x40, 0xFF));
        fig_gui_text(14, 80, dim, "%4.1f fps", fps);

        if (fig_input_scripted(1)) {
            fig_gui_panel(SCREEN_W - 58, 8, 50, 16, RGBA32(0xC0, 0x30, 0x60, 0xFF),
                           RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
            fig_gui_text(SCREEN_W - 49, 20, RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "DEMO");
        }
        fig_gui_panel(8, SCREEN_H - 24, SCREEN_W - 16, 16,
                       RGBA32(0x0C, 0x10, 0x1C, 0xFF), RGBA32(0x8B, 0x5C, 0xF6, 0xFF));
        fig_gui_text(14, SCREEN_H - 12, ink, "A punt  D< phys  D> bp  Z boxes  START stack");

        fig_gui_end();
        fig_frame_end();
    }
}
