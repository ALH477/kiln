// SPDX-License-Identifier: MIT
//
// Phase B verification: the room streaming system (kiln_room.h) with a 2×2
// grid of rooms A/B/C/D, each 80×80, sharing borders. The camera starts in
// room A and moves with the stick; the HUD shows the current room and the
// number of loaded rooms.
//
// What this exercises:
//   * AABB-overlap streaming — moving from A to B unloads A's diagonal
//     neighbour and loads B's diagonal neighbour
//   * spawn-on-load / despawn-on-unload via KilnActor.room_id
//   * per-room callback (each room builds its own coloured floor quad in
//     on_load and frees it in on_unload)
//   * brush auto-install: each room's on_load also sets room->brushes to a
//     static KilnBrush[5] (floor + 4 walls). kiln_room concatenates loaded
//     rooms' brushes into one clip world, so the player now stops at walls
//     — no kiln_clip_set_world call in this ROM.
//
// Like the other Phase B demos, geometry is hand-built. The asset pipeline
// would replace the quad with a .t3dm from gltf_to_t3d, but that's a swap of
// the load_fn body, not the room system itself.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_actor.h>
#include <kiln/kiln_room.h>
#include <kiln/kiln_clip.h>

#include <malloc.h>

#define ACTOR_POOL_CAP 32

// 12 tris of a unit cube, indexed into 8 corners. The cube geometry below
// is shared by every actor profile; rooms draw their own floor separately.
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

// ── Room floor geometry ──────────────────────────────────────────────
//
// Each room's on_load builds a vertex-coloured quad covering the room's
// AABB on Y=0. The room stores the buffer in user_mesh so on_unload can
// free it. The floor is drawn between scene_begin and actor_draw_all; the
// on_draw callback hands the buffer to t3d_vert_load + two indexed tri
// calls. (t3d_quad_draw_unindexed expects a strip — quad n uses vertices
// [n*4..n*4+3] — which doesn't match the load-once / draw-many pattern
// the actor's per-frame draw callback has, so the indexed path is simpler
// here.)

typedef struct {
    T3DVertPacked *verts;
    uint32_t rgba;
} FloorMesh;

static T3DVertPacked *make_floor_quad(fm_vec3_t aabb_min, fm_vec3_t aabb_max, uint32_t rgba)
{
    /* Four packed-vertex slots hold the four corners. Order is set so the
     * tri index pairs (0,1,2) and (0,2,3) wind CCW from above and don't
     * share a degenerate edge. */
    T3DVertPacked *v = malloc_uncached(sizeof(T3DVertPacked) * 4);
    const float x0 = aabb_min.v[0], x1 = aabb_max.v[0];
    const float z0 = aabb_min.v[2], z1 = aabb_max.v[2];
    const float y  = aabb_min.v[1];
    fm_vec3_t n_up = {{ 0, 1, 0 }};
    uint16_t np = t3d_vert_pack_normal(&n_up);
    v[0] = (T3DVertPacked){
        .posA = { x0, y, z0 }, .rgbaA = rgba, .normA = np,
        .posB = { x1, y, z0 }, .rgbaB = rgba, .normB = np,
    };
    v[1] = (T3DVertPacked){
        .posA = { x1, y, z0 }, .rgbaA = rgba, .normA = np,
        .posB = { x1, y, z1 }, .rgbaB = rgba, .normB = np,
    };
    v[2] = (T3DVertPacked){
        .posA = { x1, y, z1 }, .rgbaA = rgba, .normA = np,
        .posB = { x0, y, z1 }, .rgbaB = rgba, .normB = np,
    };
    v[3] = (T3DVertPacked){
        .posA = { x0, y, z1 }, .rgbaA = rgba, .normA = np,
        .posB = { x0, y, z0 }, .rgbaB = rgba, .normB = np,
    };
    return v;
}

// ── Actor profiles ──────────────────────────────────────────────────

enum { PROFILE_PLAYER, PROFILE_ENEMY, PROFILE_PROP, PROFILE_COUNT };

static T3DVertPacked *g_cube_player;
static T3DVertPacked *g_cube_enemy;
static T3DVertPacked *g_cube_prop;

static joypad_inputs_t g_input;

static void player_update(KilnActor *self, float dt)
{
    self->xform.pos.v[0] += (float)g_input.stick_x * 0.20f * dt * 60.0f;
    self->xform.pos.v[2] -= (float)g_input.stick_y * 0.20f * dt * 60.0f;
    self->xform.rot_angle += dt * 0.5f;
}
static void player_draw(KilnActor *self) { (void)self; draw_cube(g_cube_player); }

typedef struct { float angle, radius, speed; } EnemyState;

static void enemy_init(KilnActor *self, const KilnDict *spawn_args)
{
    (void)spawn_args;
    EnemyState *s = (EnemyState *)self->state;
    s->angle = 0.0f;
    s->radius = 12.0f;
    s->speed = 1.2f;
}
static void enemy_update(KilnActor *self, float dt)
{
    EnemyState *s = (EnemyState *)self->state;
    s->angle += s->speed * dt;
    self->xform.pos.v[0] += fm_cosf(s->angle) * s->radius * dt * 0.3f;
    self->xform.pos.v[2] += fm_sinf(s->angle) * s->radius * dt * 0.3f;
    self->xform.rot_angle = s->angle;
}
static void enemy_draw(KilnActor *self) { (void)self; draw_cube(g_cube_enemy); }

static void prop_draw(KilnActor *self) { (void)self; draw_cube(g_cube_prop); }

static const KilnActorProfile PROFILES[PROFILE_COUNT] = {
    [PROFILE_PLAYER] = { .name = "player", .category = KILN_ACTOR_CAT_PLAYER,
                          .state_size = 0, .update = player_update, .draw = player_draw },
    [PROFILE_ENEMY]  = { .name = "enemy", .category = KILN_ACTOR_CAT_ENEMY,
                          .state_size = sizeof(EnemyState),
                          .init = enemy_init, .update = enemy_update, .draw = enemy_draw },
    [PROFILE_PROP]   = { .name = "prop", .category = KILN_ACTOR_CAT_PROP,
                          .state_size = 0, .draw = prop_draw },
};

static KilnActor g_pool[ACTOR_POOL_CAP];

// The room system (mirrors kiln_actor's pool-with-handle convention — the
// storage is caller-owned, the engine only holds a static pointer to it).
static KilnRoomSystem g_sys;

// ── Rooms ───────────────────────────────────────────────────────────

#define ROOM_COUNT 4
#define MAX_LOADED 3

// A=(0,0)→(80,80) yellow, B=(80,0)→(160,80) blue, C=(0,80)→(80,160) green, D=(80,80)→(160,160) violet.
// Centre of A is (40, 0, 40) — that's where the camera spawns.
static KilnRoom g_rooms[ROOM_COUNT] = {
    [0] = {
        .id = 0,
        .aabb_min = {{   0, 0,   0 }},
        .aabb_max = {{  80, 5,  80 }},
        .spawn_count = 2,
        .spawns = {
            { PROFILE_ENEMY, {{ 40, 0, 40 }}, 0.0f },
            { PROFILE_PROP,  {{ 20, 0, 20 }}, 0.0f },
        },
        .neighbour_count = 2, .neighbours = { 1, 2 },
    },
    [1] = {
        .id = 1,
        .aabb_min = {{  80, 0,   0 }},
        .aabb_max = {{ 160, 5,  80 }},
        .spawn_count = 2,
        .spawns = {
            { PROFILE_ENEMY, {{120, 0, 40 }}, 0.0f },
            { PROFILE_PROP,  {{140, 0, 60 }}, 0.0f },
        },
        .neighbour_count = 2, .neighbours = { 0, 3 },
    },
    [2] = {
        .id = 2,
        .aabb_min = {{   0, 0,  80 }},
        .aabb_max = {{  80, 5, 160 }},
        .spawn_count = 2,
        .spawns = {
            { PROFILE_ENEMY, {{ 40, 0,120 }}, 0.0f },
            { PROFILE_PROP,  {{ 20, 0,140 }}, 0.0f },
        },
        .neighbour_count = 2, .neighbours = { 0, 3 },
    },
    [3] = {
        .id = 3,
        .aabb_min = {{  80, 0,  80 }},
        .aabb_max = {{ 160, 5, 160 }},
        .spawn_count = 2,
        .spawns = {
            { PROFILE_ENEMY, {{120, 0,120 }}, 0.0f },
            { PROFILE_PROP,  {{140, 0,100 }}, 0.0f },
        },
        .neighbour_count = 2, .neighbours = { 1, 2 },
    },
};

// One colour per room — warm yellow, cool blue, teal, violet. The player
// cube also uses yellow (the player is at the origin), so room A's tint
// should bleed into the player colour and read as "this is where I am".
static const uint32_t FLOOR_COLOURS[ROOM_COUNT] = {
    0xFFD94CFF, /* A — yellow */
    0x4C6AFFFF, /* B — blue   */
    0x00F5D4FF, /* C — teal  */
    0x8B5CF6FF, /* D — violet */
};

// One static brush array per room — floor + 4 walls. The walls are 4 units
// thick, sitting on the room's border. Designated initialisers, no malloc:
// the brushes live in .bss, kiln_room copies them into its module-static
// world buffer on load. surface 0 (default) — the demo doesn't register a
// surface table, so footstep SFX would just use whatever index 0 holds.
#define WALL_H  20.0f
#define WALL_T  4.0f

static KilnBrush make_floor_brush(fm_vec3_t aabb_min, fm_vec3_t aabb_max)
{
    KilnBrush b = { .surface = 0, .flags = 0 };
    b.mins = aabb_min;
    b.maxs = aabb_max;
    /* Floor at aabb_min.y, 1 unit thick so it has real volume. */
    b.mins.v[1] = aabb_min.v[1];
    b.maxs.v[1] = aabb_min.v[1] + 1.0f;
    return b;
}

static KilnBrush make_wall_brush(fm_vec3_t aabb_min, fm_vec3_t aabb_max, int side)
{
    KilnBrush b = { .surface = 0, .flags = 0 };
    b.mins = aabb_min; b.maxs = aabb_max;
    b.maxs.v[1] = aabb_min.v[1] + WALL_H;
    switch (side) {
    case 0: /* -X wall */ b.maxs.v[0] = b.mins.v[0] + WALL_T; break;
    case 1: /* +X wall */ b.mins.v[0] = b.maxs.v[0] - WALL_T; break;
    case 2: /* -Z wall */ b.maxs.v[2] = b.mins.v[2] + WALL_T; break;
    case 3: /* +Z wall */ b.mins.v[2] = b.maxs.v[2] - WALL_T; break;
    }
    return b;
}

// Per-room brush storage. Five brushes per room: floor + 4 walls. The
// kiln_room system copies these into its module-static clip world on load
// (and drops them on unload). room->brushes points here for the room's
// loaded lifetime; on_unload just clears the pointer.
static KilnBrush g_room_brushes[ROOM_COUNT][5];

static void room_install_brushes(KilnRoom *room)
{
    KilnBrush *bs = g_room_brushes[room->id];
    bs[0] = make_floor_brush(room->aabb_min, room->aabb_max);
    bs[1] = make_wall_brush(room->aabb_min, room->aabb_max, 0);
    bs[2] = make_wall_brush(room->aabb_min, room->aabb_max, 1);
    bs[3] = make_wall_brush(room->aabb_min, room->aabb_max, 2);
    bs[4] = make_wall_brush(room->aabb_min, room->aabb_max, 3);
    room->brushes      = bs;
    room->brush_count  = 5;
}

// ── Room callbacks ─────────────────────────────────────────────────

static void room_load(KilnRoom *room, void *user)
{
    (void)user;
    FloorMesh *m = malloc_uncached(sizeof(FloorMesh));
    m->verts = make_floor_quad(room->aabb_min, room->aabb_max, FLOOR_COLOURS[room->id]);
    m->rgba  = FLOOR_COLOURS[room->id];
    room->user_mesh = m;
    /* Install this room's brushes; kiln_room will copy them into the clip
     * world right after this callback returns. */
    room_install_brushes(room);
}

static void room_unload(KilnRoom *room, void *user)
{
    (void)user;
    if (room->user_mesh) {
        FloorMesh *m = (FloorMesh *)room->user_mesh;
        free_uncached(m->verts);
        free_uncached(m);
        room->user_mesh = NULL;
    }
    /* Brushes are in .bss, nothing to free. Just drop the pointer. */
    room->brushes = NULL;
    room->brush_count = 0;
}

static void room_spawn(KilnRoom *room, const KilnRoomSpawn *spawn, void *user)
{
    (void)user;
    kiln_actor_spawn_in_room(spawn->profile_id, spawn->pos, spawn->yaw, room->id, &spawn->dict);
}

static void room_draw(KilnRoom *room, void *user)
{
    (void)user;
    if (!room->user_mesh) return;
    FloorMesh *m = (FloorMesh *)room->user_mesh;
    t3d_vert_load(m->verts, 0, 4);
    t3d_tri_draw(0, 1, 2);
    t3d_tri_draw(0, 2, 3);
    t3d_tri_sync();
}

// ── Camera path ────────────────────────────────────────────────────

// 2×2 grid origin in the middle so the camera can move freely.
#define WORLD_MIN_X  -10
#define WORLD_MAX_X  170
#define WORLD_MIN_Z  -10
#define WORLD_MAX_Z  170

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();

    g_cube_player = make_color_cube(8, 0xFFD94CFF);
    g_cube_enemy  = make_color_cube(6, 0x4C6AFFFF);
    g_cube_prop   = make_color_cube(10, 0x00F5D4FF);

    kiln_actor_system_init(PROFILES, PROFILE_COUNT, g_pool, ACTOR_POOL_CAP);

    kiln_room_system_init(&g_sys, g_rooms, ROOM_COUNT, MAX_LOADED,
                          room_load, room_unload, room_spawn, room_draw, NULL,
                          /*owns_clip_world=*/1);

    // The player is *not* in any room — it has room_id == KILN_ACTOR_ROOM_NONE
    // and is never auto-despawned by the room system.
    kiln_actor_spawn(PROFILE_PLAYER, (fm_vec3_t){{ 40, 0, 40 }}, 0.0f, NULL);

    KilnScene scene;
    kiln_scene_init(&scene);
    scene.far_z = 400.0f;
    scene.fov_deg = 70.0f;

    // The camera follows the player from a fixed offset; this is the same
    // follow pattern the actor-demo uses. A real game would have collision
    // pushing the camera against walls, but that's kiln_collision's job.
    fm_vec3_t player_pos = {{ 40, 0, 40 }};
    scene.cam_pos    = (fm_vec3_t){{ 40, 60, -70 }};
    scene.cam_target = (fm_vec3_t){{ 40,  0,  40 }};
    kiln_scene_update(&scene);

    uint32_t frames = 0;
    float fps = 0.0f;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        joypad_poll();
        g_input = joypad_get_inputs(JOYPAD_PORT_1);

        float dt = 1.0f / 60.0f;

        // Move the player (room-less; the room system doesn't track it).
        // The walls installed by room_load now stop the player — the
        // WORLD_MIN/MAX clamps are gone, kiln_clip_slide handles it. The
        // player is a 16-unit cube (half=8); 4 slide iterations is the
        // convention from kiln_player.
        fm_vec3_t half = {{ 8, 8, 8 }};
        fm_vec3_t disp = {{ (float)g_input.stick_x * 0.25f * dt * 60.0f,
                            0.0f,
                           -(float)g_input.stick_y * 0.25f * dt * 60.0f }};
        player_pos = kiln_clip_slide(player_pos, disp, half, half, 4);

        KilnActor *player = kiln_actor_first(KILN_ACTOR_CAT_PLAYER);
        if (player) player->xform.pos = player_pos;

        kiln_actor_update_all(dt);
        kiln_room_system_update(&g_sys, player_pos);

        scene.cam_pos = (fm_vec3_t){{ player_pos.v[0], 60, player_pos.v[2] - 70 }};
        scene.cam_target = player_pos;
        kiln_scene_update(&scene);

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        kiln_frame_begin();
        kiln_scene_begin(&scene);
        kiln_room_draw_all(&g_sys);
        kiln_actor_draw_all();

        kiln_gui_begin();
        kiln_gui_panel(8, 8, 168, 84,
                      RGBA32(10, 10, 24, 200), RGBA32(0, 245, 212, 255));
        kiln_gui_text(14, 22, RGBA32(0, 245, 212, 255), "KILN ROOMS");

        KilnRoom *cur = kiln_room_current(&g_sys);
        char room_label[8] = "—";
        if (cur) room_label[0] = (char)('A' + cur->id);
        kiln_gui_text(14, 34, RGBA32(232, 232, 240, 255),
                     "room    %s", room_label);
        kiln_gui_text(14, 46, RGBA32(232, 232, 240, 255),
                     "loaded  %u / %u",
                     kiln_room_loaded_count(&g_sys), MAX_LOADED);
        kiln_gui_text(14, 58, RGBA32(232, 232, 240, 255),
                     "enemy %u  prop %u",
                     kiln_actor_count(KILN_ACTOR_CAT_ENEMY),
                     kiln_actor_count(KILN_ACTOR_CAT_PROP));
        kiln_gui_text(14, 70, RGBA32(232, 232, 240, 255),
                     "fps %5.1f", fps);
        kiln_gui_text(14, 82, RGBA32(232, 232, 240, 255),
                     "stick: move player");
        kiln_gui_end();

        kiln_frame_end();
    }
}