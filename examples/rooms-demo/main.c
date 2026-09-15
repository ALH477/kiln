// SPDX-License-Identifier: MIT
//
// kiln_room streaming: four rooms in a 2x2 grid, joined by doorways, loaded and
// unloaded around the player as they walk through them.
//
//   A B      each room is 160 x 160 with its own floor tint, a pillar, and an
//   C D      enemy that exists only while its room is loaded
//
// What it shows:
//   * the loaded set follows the player — the room they are in plus its
//     edge neighbours, never the diagonal, never more than MAX_LOADED
//   * spawn-on-load / despawn-on-unload via KilnActor.room_id (watch the enemy
//     count and the minimap as a room drops out)
//   * brush auto-install: each room's on_load hands kiln_room its floor, walls
//     and pillar, and kiln_room builds the clip world from whatever is loaded
//
// A shared wall is two half-thickness stubs, one owned by each room, with a
// doorway between. The old demo gave every room four full walls, so the two
// halves of a shared wall formed a doorless double wall and the player could
// never leave room A — which is also why kiln_room's loaded set could grow
// until it asserted without anyone seeing a room load or unload.
//
//   stick  walk      Z  room outlines      idle 2 s: a tour of all four rooms
//
// Jump ROM: .#rooms-demo-room-d boots standing in room D.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_actor.h>
#include <kiln/kiln_room.h>
#include <kiln/kiln_clip.h>
#include <kiln/kiln_prim.h>
#include <kiln/kiln_debugdraw.h>

enum { JUMP_NONE, JUMP_ROOM_D };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W 320
#define SCREEN_H 240
#define ACTOR_POOL_CAP 16
#define ROOM_COUNT 4
#define MAX_LOADED 3
#define ROOM_SIZE 160.0f
#define WALL_H 36.0f
#define WALL_T 4.0f
#define DOOR_HALF 24.0f
#define FLOOR_TOP 1.0f
#define WALK_SPEED 110.0f

static const fm_vec3_t P_MINS = {{ -8, -10, -8 }};
static const fm_vec3_t P_MAXS = {{  8,  10,  8 }};

static const uint8_t ROOM_TINT[ROOM_COUNT][3] = {
    { 0xD8, 0xB0, 0x48 },   /* A amber  */
    { 0x50, 0x78, 0xD8 },   /* B blue   */
    { 0x38, 0xB8, 0x98 },   /* C teal   */
    { 0x98, 0x60, 0xD0 },   /* D violet */
};
static const char ROOM_NAME[ROOM_COUNT] = { 'A', 'B', 'C', 'D' };

// ── Enemy: orbits the point it spawned at, and only while its room is loaded ─
enum { PROFILE_ENEMY, PROFILE_COUNT };
typedef struct { float angle; fm_vec3_t centre; } EnemyState;

static KilnPrim g_enemy_prim[ROOM_COUNT];

static void enemy_init(KilnActor *self, const KilnDict *args)
{
    (void)args;
    EnemyState *s = (EnemyState *)self->state;
    s->centre = self->xform.pos;
    s->angle = self->xform.pos.v[0] * 0.05f;
    self->xform.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
}

static void enemy_update(KilnActor *self, float dt)
{
    EnemyState *s = (EnemyState *)self->state;
    s->angle += 1.6f * dt;
    /* Around the spawn point, not drifting from wherever it is now. */
    self->xform.pos.v[0] = s->centre.v[0] + fm_cosf(s->angle) * 22.0f;
    self->xform.pos.v[2] = s->centre.v[2] + fm_sinf(s->angle) * 22.0f;
    self->xform.pos.v[1] = s->centre.v[1] + 3.0f * fm_sinf(s->angle * 3.0f);
    self->xform.rot_angle = -s->angle;
}

static void enemy_draw(KilnActor *self)
{
    const uint8_t room = self->room_id < ROOM_COUNT ? self->room_id : 0;
    kiln_prim_draw(&g_enemy_prim[room]);
}

static const KilnActorProfile PROFILES[PROFILE_COUNT] = {
    [PROFILE_ENEMY] = { .name = "enemy", .category = KILN_ACTOR_CAT_ENEMY,
                        .state_size = sizeof(EnemyState),
                        .init = enemy_init, .update = enemy_update, .draw = enemy_draw },
};
static KilnActor g_pool[ACTOR_POOL_CAP];

// ── Rooms ───────────────────────────────────────────────────────────────
#define ROOM(i, x, z, n0, n1) {                                              \
    .id = i,                                                                 \
    .aabb_min = {{ x, 0, z }}, .aabb_max = {{ x + ROOM_SIZE, 60, z + ROOM_SIZE }}, \
    .spawn_count = 1,                                                        \
    .spawns = { { .profile_id = PROFILE_ENEMY,                               \
                  .pos = {{ x + 120, FLOOR_TOP + 10, z + 120 }} } },         \
    .neighbour_count = 2, .neighbours = { n0, n1 },                          \
}
static KilnRoom g_rooms[ROOM_COUNT] = {
    ROOM(0,   0,   0, 1, 2),
    ROOM(1, 160,   0, 0, 3),
    ROOM(2,   0, 160, 0, 3),
    ROOM(3, 160, 160, 1, 2),
};

#define MAX_ROOM_BRUSHES 8
typedef struct {
    KilnBrush brushes[MAX_ROOM_BRUSHES];
    KilnPrim  prims[MAX_ROOM_BRUSHES];
    KilnPrim  floor;
    int       count;
} RoomGeo;
static RoomGeo g_geo[ROOM_COUNT];
static int g_loads, g_unloads;

static void add_brush(RoomGeo *g, float x0, float y0, float z0, float x1, float y1, float z1)
{
    if (g->count >= MAX_ROOM_BRUSHES) return;
    g->brushes[g->count++] = (KilnBrush){ .mins = {{ x0, y0, z0 }}, .maxs = {{ x1, y1, z1 }} };
}

/* One side of a room: a full wall on the grid's outside edge, or — on a
 * border shared with a neighbour — this room's half of the wall, split around
 * a doorway. `axis` 0 is a wall at constant x, 2 at constant z. */
static void add_side(RoomGeo *g, const KilnRoom *r, int axis, int high, int shared)
{
    const fm_vec3_t mn = r->aabb_min, mx = r->aabb_max;
    const float t = WALL_T;
    if (axis == 0) {
        const float x0 = high ? mx.v[0] - t : mn.v[0];
        const float x1 = high ? mx.v[0] : mn.v[0] + t;
        if (!shared) { add_brush(g, x0, 0, mn.v[2], x1, WALL_H, mx.v[2]); return; }
        const float mid = (mn.v[2] + mx.v[2]) * 0.5f;
        add_brush(g, x0, 0, mn.v[2], x1, WALL_H, mid - DOOR_HALF);
        add_brush(g, x0, 0, mid + DOOR_HALF, x1, WALL_H, mx.v[2]);
    } else {
        const float z0 = high ? mx.v[2] - t : mn.v[2];
        const float z1 = high ? mx.v[2] : mn.v[2] + t;
        if (!shared) { add_brush(g, mn.v[0], 0, z0, mx.v[0], WALL_H, z1); return; }
        const float mid = (mn.v[0] + mx.v[0]) * 0.5f;
        add_brush(g, mn.v[0], 0, z0, mid - DOOR_HALF, WALL_H, z1);
        add_brush(g, mid + DOOR_HALF, 0, z0, mx.v[0], WALL_H, z1);
    }
}

static void room_load(KilnRoom *room, void *user)
{
    (void)user;
    RoomGeo *g = &g_geo[room->id];
    g->count = 0;
    const fm_vec3_t mn = room->aabb_min;
    const int east = mn.v[0] > 1.0f, south = mn.v[2] > 1.0f;   /* which cell */

    /* Floor slab (collision only; the checker below is what you see). */
    add_brush(g, mn.v[0], -8, mn.v[2], mn.v[0] + ROOM_SIZE, FLOOR_TOP, mn.v[2] + ROOM_SIZE);
    add_side(g, room, 0, 0, east);     /* -X side is shared when this is an east room  */
    add_side(g, room, 0, 1, !east);    /* +X side is shared when this is a west room   */
    add_side(g, room, 2, 0, south);
    add_side(g, room, 2, 1, !south);
    /* A pillar in the quadrant the tour never crosses. */
    add_brush(g, mn.v[0] + 28, 0, mn.v[2] + 28, mn.v[0] + 52, 44, mn.v[2] + 52);

    const uint8_t *c = ROOM_TINT[room->id];
    const uint32_t wall_top = kiln_prim_rgba((uint8_t)(0x90 + c[0] / 4), (uint8_t)(0x90 + c[1] / 4),
                                             (uint8_t)(0x90 + c[2] / 4));
    const uint32_t wall_side = kiln_prim_rgba((uint8_t)(0x40 + c[0] / 4), (uint8_t)(0x40 + c[1] / 4),
                                              (uint8_t)(0x48 + c[2] / 4));
    for (int i = 1; i < g->count; i++) {
        const KilnBrush *b = &g->brushes[i];
        const int pillar = (i == g->count - 1);
        kiln_prim_box(&g->prims[i],
                      (fm_vec3_t){{ (b->mins.v[0] + b->maxs.v[0]) * 0.5f, (b->mins.v[1] + b->maxs.v[1]) * 0.5f,
                                    (b->mins.v[2] + b->maxs.v[2]) * 0.5f }},
                      (fm_vec3_t){{ (b->maxs.v[0] - b->mins.v[0]) * 0.5f, (b->maxs.v[1] - b->mins.v[1]) * 0.5f,
                                    (b->maxs.v[2] - b->mins.v[2]) * 0.5f }},
                      pillar ? kiln_prim_rgba(c[0], c[1], c[2]) : wall_top,
                      pillar ? kiln_prim_shade(kiln_prim_rgba(c[0], c[1], c[2]), 0.6f) : wall_side,
                      kiln_prim_rgba(0x18, 0x18, 0x20));
    }
    kiln_prim_floor(&g->floor, ROOM_SIZE * 0.5f, 8,
                    kiln_prim_rgba((uint8_t)(c[0] / 2 + 0x18), (uint8_t)(c[1] / 2 + 0x18), (uint8_t)(c[2] / 2 + 0x18)),
                    kiln_prim_rgba((uint8_t)(c[0] / 3 + 0x14), (uint8_t)(c[1] / 3 + 0x14), (uint8_t)(c[2] / 3 + 0x14)));

    room->brushes = g->brushes;
    room->brush_count = (uint16_t)g->count;
    room->user_mesh = g;
    g_loads++;
}

static void room_unload(KilnRoom *room, void *user)
{
    (void)user;
    RoomGeo *g = &g_geo[room->id];
    for (int i = 1; i < g->count; i++) kiln_prim_free(&g->prims[i]);
    kiln_prim_free(&g->floor);
    g->count = 0;
    room->user_mesh = NULL;
    room->brushes = NULL;
    room->brush_count = 0;
    g_unloads++;
}

static void room_spawn(KilnRoom *room, const KilnRoomSpawn *spawn, void *user)
{
    (void)user;
    kiln_actor_spawn_in_room(spawn->profile_id, spawn->pos, spawn->yaw, room->id, &spawn->dict);
}

static void room_draw(KilnRoom *room, void *user)
{
    (void)user;
    const RoomGeo *g = (const RoomGeo *)room->user_mesh;
    if (!g) return;
    static KilnTransform xf;
    static int ready;
    if (!ready) { kiln_transform_init(&xf); ready = 1; }
    xf.pos = (fm_vec3_t){{ room->aabb_min.v[0] + ROOM_SIZE * 0.5f, FLOOR_TOP,
                           room->aabb_min.v[2] + ROOM_SIZE * 0.5f }};
    kiln_transform_push(&xf);
    kiln_prim_draw(&g->floor);
    kiln_transform_pop();
    for (int i = 1; i < g->count; i++) kiln_prim_draw(&g->prims[i]);
}

static KilnRoomSystem g_sys;

// ── Tour ────────────────────────────────────────────────────────────────
// A -> B -> D -> C -> A through the four doorways. Stick counts are raw; the
// camera looks down +Z, so stick left walks +X. Each leg is one room width at
// full tilt, and opposite legs are equal, so the loop closes on itself.
static const KilnInputKey TOUR_KEYS[] = {
    { .frame =   0, .sx = -85 },
    { .frame =  92, .sy =  85 },
    { .frame = 184, .sx =  85 },
    { .frame = 276, .sy = -85 },
    { .frame = 368 },
};
static const KilnInputTape TOUR = { TOUR_KEYS, 5, 0 };

static color_t tint(int room, float k)
{
    return RGBA32((uint8_t)(ROOM_TINT[room][0] * k), (uint8_t)(ROOM_TINT[room][1] * k),
                  (uint8_t)(ROOM_TINT[room][2] * k), 0xFF);
}

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    kiln_input_init();

    for (int i = 0; i < ROOM_COUNT; i++) {
        const uint8_t *c = ROOM_TINT[i];
        kiln_prim_box(&g_enemy_prim[i], (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 7, 7, 7 }},
                      kiln_prim_rgba(0xFF, 0xFF, 0xFF), kiln_prim_rgba(c[0], c[1], c[2]),
                      kiln_prim_shade(kiln_prim_rgba(c[0], c[1], c[2]), 0.4f));
    }
    KilnPrim body, nose, shadow;
    kiln_prim_box(&body, (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 8, 10, 8 }},
                  kiln_prim_rgba(0xFF, 0x70, 0x60), kiln_prim_rgba(0xE0, 0x48, 0x40),
                  kiln_prim_rgba(0x60, 0x10, 0x10));
    kiln_prim_box(&nose, (fm_vec3_t){{ 0, 3, 10 }}, (fm_vec3_t){{ 3, 3, 3 }},
                  kiln_prim_rgba(0xFF, 0xFF, 0xFF), kiln_prim_rgba(0xE0, 0xE0, 0xE8),
                  kiln_prim_rgba(0x80, 0x80, 0x80));
    kiln_prim_floor(&shadow, 11.0f, 1, kiln_prim_rgba(0x10, 0x10, 0x14), kiln_prim_rgba(0x10, 0x10, 0x14));
    KilnTransform body_xf, shadow_xf;
    kiln_transform_init(&body_xf);
    kiln_transform_init(&shadow_xf);
    body_xf.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};

    kiln_actor_system_init(PROFILES, PROFILE_COUNT, g_pool, ACTOR_POOL_CAP);
    kiln_room_system_init(&g_sys, g_rooms, ROOM_COUNT, MAX_LOADED,
                          room_load, room_unload, room_spawn, room_draw, NULL,
                          /*owns_clip_world=*/1);

    fm_vec3_t pos = {{ 80, FLOOR_TOP + 10.5f, 80 }};
    if (KILN_JUMP == JUMP_ROOM_D) pos = (fm_vec3_t){{ 240, FLOOR_TOP + 10.5f, 240 }};
    else kiln_input_set_attract(1, &TOUR, 120);
    float yaw = 0.0f;
    int outlines = 1;

    KilnScene scene;
    kiln_scene_init(&scene);
    kiln_prim_stage(&scene, RGBA32(0x14, 0x16, 0x22, 0xFF), 200.0f, 360.0f);
    scene.fov_deg = 60.0f;
    scene.near_z = 16.0f;
    scene.far_z = 360.0f;

    uint32_t frames = 0;
    float fps = 60.0f;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);
        const float dt = 1.0f / 60.0f;
        if (in->edges & KILN_BTN_Z) outlines = !outlines;

        /* The room system updates FIRST, on last frame's position, so the
         * clip world this frame's move slides against is the one around the
         * player — not one that could still be missing the room they walk into. */
        kiln_room_system_update(&g_sys, pos);

        const fm_vec3_t disp = {{ -in->stick_x * WALK_SPEED * dt, 0, in->stick_y * WALK_SPEED * dt }};
        pos = kiln_clip_slide(pos, disp, P_MINS, P_MAXS, 4);
        if (disp.v[0] * disp.v[0] + disp.v[2] * disp.v[2] > 1e-4f) yaw = fm_atan2f(disp.v[0], disp.v[2]);

        kiln_actor_update_all(dt);

        scene.cam_target = (fm_vec3_t){{ pos.v[0], 0, pos.v[2] + 24 }};
        scene.cam_pos = (fm_vec3_t){{ pos.v[0], 150, pos.v[2] - 120 }};
        kiln_scene_update(&scene);

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        /* ── 3D ───────────────────────────────────────────────────── */
        kiln_frame_begin();
        kiln_scene_begin(&scene);
        kiln_room_draw_all(&g_sys);
        kiln_actor_draw_all();

        shadow_xf.pos = (fm_vec3_t){{ pos.v[0], FLOOR_TOP + 0.4f, pos.v[2] }};
        kiln_transform_push(&shadow_xf);
        kiln_prim_draw(&shadow);
        kiln_transform_pop();
        body_xf.pos = pos;
        body_xf.rot_angle = yaw;
        kiln_transform_push(&body_xf);
        kiln_prim_draw(&body);
        kiln_prim_draw(&nose);
        kiln_transform_pop();

        /* ── 2D ───────────────────────────────────────────────────── */
        kiln_gui_begin();

        const KilnRoom *cur = kiln_room_current(&g_sys);

        if (outlines) {
            kiln_dd_begin(&scene, SCREEN_W, SCREEN_H);
            for (KilnRoom *r = kiln_room_first_loaded(&g_sys); r; r = kiln_room_next_loaded(&g_sys, r)) {
                const fm_vec3_t top = {{ r->aabb_max.v[0], WALL_H + 2, r->aabb_max.v[2] }};
                kiln_dd_aabb((fm_vec3_t){{ r->aabb_min.v[0], WALL_H + 2, r->aabb_min.v[2] }}, top,
                             tint(r->id, r == cur ? 1.2f : 0.7f));
                kiln_dd_text((fm_vec3_t){{ r->aabb_min.v[0] + ROOM_SIZE * 0.5f, WALL_H + 20,
                                           r->aabb_min.v[2] + ROOM_SIZE * 0.5f }},
                             RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "%c", ROOM_NAME[r->id]);
            }
            kiln_dd_end();
        }

        const color_t ink = RGBA32(0xE8, 0xE8, 0xF0, 0xFF);
        const color_t teal = RGBA32(0x00, 0xF5, 0xD4, 0xFF);
        kiln_gui_panel(8, 8, 142, 66, RGBA32(0x0C, 0x10, 0x1C, 0xFF), teal);
        kiln_gui_text(14, 21, teal, "KILN ROOMS");
        kiln_gui_text(14, 34, ink, "room %c  loaded %u/%d", cur ? ROOM_NAME[cur->id] : '-',
                      kiln_room_loaded_count(&g_sys), MAX_LOADED);
        kiln_gui_text(14, 46, ink, "loads %d  unloads %d", g_loads, g_unloads);
        kiln_gui_text(14, 58, ink, "enemies %u", kiln_actor_count(KILN_ACTOR_CAT_ENEMY));
        kiln_gui_text(14, 70, RGBA32(0x90, 0x98, 0xB0, 0xFF), "%4.1f fps", fps);

        /* Minimap: lit = loaded, outlined = the room you are in. Screen x is
         * mirrored against world x because the camera looks down +Z. */
        const int mx0 = SCREEN_W - 58, my0 = 30, cell = 24;
        kiln_gui_rect(mx0 - 3, my0 - 3, cell * 2 + 6, cell * 2 + 6, RGBA32(0x0C, 0x10, 0x1C, 0xFF));
        for (int i = 0; i < ROOM_COUNT; i++) {
            const int col = 1 - (int)(g_rooms[i].aabb_min.v[0] / ROOM_SIZE);
            const int row = 1 - (int)(g_rooms[i].aabb_min.v[2] / ROOM_SIZE);
            const int x = mx0 + col * cell, y = my0 + row * cell;
            if (cur && cur->id == i) kiln_gui_rect(x, y, cell, cell, RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
            kiln_gui_rect(x + 2, y + 2, cell - 4, cell - 4,
                          kiln_room_loaded(&g_sys, (uint8_t)i) ? tint(i, 1.0f) : RGBA32(0x28, 0x2A, 0x34, 0xFF));
        }
        const int px = mx0 + (int)((2.0f - pos.v[0] / ROOM_SIZE) * cell);
        const int py = my0 + (int)((2.0f - pos.v[2] / ROOM_SIZE) * cell);
        kiln_gui_rect(px - 2, py - 2, 4, 4, RGBA32(0xFF, 0x40, 0x40, 0xFF));

        if (kiln_input_scripted(1)) {
            kiln_gui_panel(SCREEN_W - 58, 88, 50, 16, RGBA32(0xC0, 0x30, 0x60, 0xFF),
                           RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
            kiln_gui_text(SCREEN_W - 49, 100, RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "DEMO");
        }
        kiln_gui_panel(8, SCREEN_H - 24, SCREEN_W - 16, 16,
                       RGBA32(0x0C, 0x10, 0x1C, 0xFF), RGBA32(0x8B, 0x5C, 0xF6, 0xFF));
        kiln_gui_text(14, SCREEN_H - 12, ink, "stick walk   Z outlines   idle 2s: tour");

        kiln_gui_end();
        kiln_frame_end();
    }
}
