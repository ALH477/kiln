// SPDX-License-Identifier: MIT
//
// Phase E integration proof: the kiln_physics module + kiln_room brush
// auto-install + kiln_clip broadphase toggle, in one frame.
//
//   kiln_room     -> one room, on_load populates room->brushes (floor + 4 walls)
//                   kiln_room copies them into the clip world automatically —
//                   no kiln_clip_set_world call in this ROM
//   kiln_clip     -> swept-AABB vs the auto-installed brushes; broadphase grid
//                   toggleable at runtime via kiln_clip_set_broadphase
//   kiln_physics  -> 6 dynamic crate bodies fall, stack, rest, sleep; A punts
//                   the nearest crate in a forward cone (the gravity-gun feel);
//                   kiln_physics_set_enabled toggles the whole engine on/off
//
// HUD: fps, body count, sleeping count, PHYS ON/OFF, BP ON/OFF, last trace
// brush count (so the broadphase win is visible — flat walk = total brushes,
// grid = brushes in overlapped cells only).
//
// Controls:
//   stick       move player (kiln_clip_slide against the auto-installed walls)
//   A           punt nearest crate in the player's forward cone
//   D-pad left  toggle PHYS ON/OFF (kiln_physics_set_enabled)
//   D-pad right toggle BP ON/OFF   (kiln_clip_set_broadphase)

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_clip.h>
#include <kiln/kiln_actor.h>
#include <kiln/kiln_room.h>
#include <kiln/kiln_physics.h>

#include <malloc.h>

#define SCREEN_W 320
#define SCREEN_H 240
#define ACTOR_POOL_CAP 8
#define BODY_CAP 16

// ── Cube geometry ───────────────────────────────────────────────────────
static const uint8_t CUBE_TRIS[12][3] = {
    {0,1,2},{2,3,0}, {4,6,5},{6,4,7},
    {0,4,5},{5,1,0}, {1,5,6},{6,2,1},
    {2,6,7},{7,3,2}, {3,7,4},{4,0,3},
};

/* Build a unit-cube vertex buffer (half-extent = 1 in model space) that we
 * scale per-crate via a pushed SRT matrix. 8 verts packed into 4 T3DVertPacked
 * structs, the same layout kiln_actor uses. */
static T3DVertPacked *make_unit_cube(uint32_t rgba)
{
    T3DVertPacked *v = malloc_uncached(sizeof(T3DVertPacked) * 4);
    const float c[8][3] = {
        {-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1},
        {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1},
    };
    for (int i = 0; i < 8; i += 2) {
        fm_vec3_t na = {{ c[i][0],   c[i][1],   c[i][2]   }};
        fm_vec3_t nb = {{ c[i+1][0], c[i+1][1], c[i+1][2] }};
        fm_vec3_norm(&na, &na);
        fm_vec3_norm(&nb, &nb);
        v[i / 2] = (T3DVertPacked){
            .posA = { (int16_t)c[i][0],   (int16_t)c[i][1],   (int16_t)c[i][2]   },
            .rgbaA = rgba, .normA = t3d_vert_pack_normal(&na),
            .posB = { (int16_t)c[i+1][0], (int16_t)c[i+1][1], (int16_t)c[i+1][2] },
            .rgbaB = rgba, .normB = t3d_vert_pack_normal(&nb),
        };
    }
    return v;
}

static void draw_cube_mesh(T3DVertPacked *v)
{
    t3d_vert_load(v, 0, 8);
    for (int i = 0; i < 12; i++)
        t3d_tri_draw(CUBE_TRIS[i][0], CUBE_TRIS[i][1], CUBE_TRIS[i][2]);
    t3d_tri_sync();
}

/* Build a box vertex buffer at a world-space AABB. Used for the floor and
 * walls — world-space verts, no matrix push needed at draw time. 8 verts
 * packed into 4 T3DVertPacked. */
static T3DVertPacked *make_world_box(float mn0, float mn1, float mn2,
                                     float mx0, float mx1, float mx2,
                                     uint32_t rgba)
{
    T3DVertPacked *v = malloc_uncached(sizeof(T3DVertPacked) * 4);
    const float c[8][3] = {
        {mn0, mn1, mn2}, {mx0, mn1, mn2}, {mx0, mx1, mn2}, {mn0, mx1, mn2},
        {mn0, mn1, mx2}, {mx0, mn1, mx2}, {mx0, mx1, mx2}, {mn0, mx1, mx2},
    };
    for (int i = 0; i < 8; i += 2) {
        fm_vec3_t na = {{ c[i][0],   c[i][1],   c[i][2]   }};
        fm_vec3_t nb = {{ c[i+1][0], c[i+1][1], c[i+1][2] }};
        fm_vec3_norm(&na, &na);
        fm_vec3_norm(&nb, &nb);
        v[i / 2] = (T3DVertPacked){
            .posA = { (int16_t)c[i][0],   (int16_t)c[i][1],   (int16_t)c[i][2]   },
            .rgbaA = rgba, .normA = t3d_vert_pack_normal(&na),
            .posB = { (int16_t)c[i+1][0], (int16_t)c[i+1][1], (int16_t)c[i+1][2] },
            .rgbaB = rgba, .normB = t3d_vert_pack_normal(&nb),
        };
    }
    return v;
}

static T3DVertPacked *g_cube_player;
static T3DVertPacked *g_unit_crate;
static T3DMat4FP     *g_matfp; /* reused per crate per frame */

// ── Player ──────────────────────────────────────────────────────────────
// A non-physics actor: stick → kiln_clip_slide against the auto-installed
// walls. kiln_physics is for crates; the player is its own locomotion so the
// state machine and feel stay tight. See kiln_physics.h's "for non-player
// actors" note.
typedef struct { float yaw; } PlayerState;

static void player_init(KilnActor *self, const KilnDict *args)
{
    (void)args;
    ((PlayerState *)self->state)->yaw = 0.0f;
}

static void player_update(KilnActor *self, float dt)
{
    PlayerState *s = (PlayerState *)self->state;
    const KilnInput *in = kiln_input_get(1);

    float dx =  (float)in->stick_x * 0.30f * dt * 60.0f;
    float dz = -(float)in->stick_y * 0.30f * dt * 60.0f;
    fm_vec3_t half = {{ 8, 8, 8 }};
    fm_vec3_t disp = {{ dx, 0, dz }};
    self->xform.pos = kiln_clip_slide(self->xform.pos, disp, half, half, 4);

    if (dx*dx + dz*dz > 1.0f) {
        s->yaw = atan2f(dx, -dz);
        self->xform.rot_angle = s->yaw;
    }
}

static void player_draw(KilnActor *self) { (void)self; draw_cube_mesh(g_cube_player); }

static const KilnActorProfile PROFILES[1] = {
    { .name = "player", .category = KILN_ACTOR_CAT_PLAYER,
      .state_size = sizeof(PlayerState),
      .init = player_init, .update = player_update, .draw = player_draw },
};
enum { PROFILE_PLAYER = 0, PROFILE_COUNT = 1 };

static KilnActor g_pool[ACTOR_POOL_CAP];

// ── Room: floor + 4 walls ────────────────────────────────────────────────
//
// One room, 200×200, walls 40 tall and 4 thick. Brushes live in .bss;
// kiln_room copies them into its module-static clip world right after
// on_load returns. No kiln_clip_set_world call anywhere in this ROM.
//
// The visible mesh is also pre-built: floor + 4 wall boxes as world-space
// vertex buffers, drawn directly (no matrix push). The brush array and the
// mesh are built from the same AABBs so what you see is what you collide with.

#define ROOM_HALF 100.0f
#define WALL_H    40.0f
#define WALL_T    4.0f
#define FLOOR_H   1.0f

static KilnBrush g_room_brushes[5];
static T3DVertPacked *g_room_mesh[5];

static void room_load(KilnRoom *room, void *user)
{
    (void)user;
    fm_vec3_t mn = room->aabb_min, mx = room->aabb_max;

    g_room_brushes[0] = (KilnBrush){
        .mins = {{ mn.v[0], mn.v[1], mn.v[2] }},
        .maxs = {{ mx.v[0], mn.v[1] + FLOOR_H, mx.v[2] }},
        .surface = 0, .flags = 0 };
    g_room_brushes[1] = (KilnBrush){
        .mins = {{ mn.v[0],           mn.v[1], mn.v[2] }},
        .maxs = {{ mn.v[0] + WALL_T,  mn.v[1] + WALL_H, mx.v[2] }}, .surface = 0, .flags = 0 };
    g_room_brushes[2] = (KilnBrush){
        .mins = {{ mx.v[0] - WALL_T,  mn.v[1], mn.v[2] }},
        .maxs = {{ mx.v[0],           mn.v[1] + WALL_H, mx.v[2] }}, .surface = 0, .flags = 0 };
    g_room_brushes[3] = (KilnBrush){
        .mins = {{ mn.v[0],           mn.v[1], mn.v[2] }},
        .maxs = {{ mx.v[0],           mn.v[1] + WALL_H, mn.v[2] + WALL_T }}, .surface = 0, .flags = 0 };
    g_room_brushes[4] = (KilnBrush){
        .mins = {{ mn.v[0],           mn.v[1], mx.v[2] - WALL_T }},
        .maxs = {{ mx.v[0],           mn.v[1] + WALL_H, mx.v[2] }}, .surface = 0, .flags = 0 };

    room->brushes = g_room_brushes;
    room->brush_count = 5;

    /* Match the visible mesh to the brushes — same AABBs. */
    g_room_mesh[0] = make_world_box(mn.v[0], mn.v[1], mn.v[2],
                                    mx.v[0], mn.v[1] + FLOOR_H, mx.v[2], 0x303040FF);
    g_room_mesh[1] = make_world_box(mn.v[0], mn.v[1], mn.v[2],
                                    mn.v[0] + WALL_T, mn.v[1] + WALL_H, mx.v[2], 0x505060FF);
    g_room_mesh[2] = make_world_box(mx.v[0] - WALL_T, mn.v[1], mn.v[2],
                                    mx.v[0], mn.v[1] + WALL_H, mx.v[2], 0x505060FF);
    g_room_mesh[3] = make_world_box(mn.v[0], mn.v[1], mn.v[2],
                                    mx.v[0], mn.v[1] + WALL_H, mn.v[2] + WALL_T, 0x505060FF);
    g_room_mesh[4] = make_world_box(mn.v[0], mn.v[1], mx.v[2] - WALL_T,
                                    mx.v[0], mn.v[1] + WALL_H, mx.v[2], 0x505060FF);
}

static void room_unload(KilnRoom *room, void *user)
{
    (void)user;
    for (int i = 0; i < 5; i++) {
        if (g_room_mesh[i]) {
            free_uncached(g_room_mesh[i]);
            g_room_mesh[i] = NULL;
        }
    }
    room->brushes = NULL;
    room->brush_count = 0;
}

static void room_spawn(KilnRoom *room, const KilnRoomSpawn *spawn, void *user)
{
    (void)room; (void)spawn; (void)user;
}

static void room_draw(KilnRoom *room, void *user)
{
    (void)user; (void)room;
    for (int i = 0; i < 5; i++) {
        if (g_room_mesh[i]) draw_cube_mesh(g_room_mesh[i]);
    }
}

static KilnRoomSystem g_sys;
static KilnRoom g_room = {
    .id = 0,
    .aabb_min = {{ -ROOM_HALF, 0, -ROOM_HALF }},
    .aabb_max = {{  ROOM_HALF, 0,  ROOM_HALF }},
    .neighbour_count = 0,
    .spawn_count = 0,
};

// ── Physics world + crates ──────────────────────────────────────────────
static KilnPhysicsBody g_bodies[BODY_CAP];
static KilnPhysicsWorld g_phys;

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);
    kiln_input_init();

    g_cube_player = make_world_box(-8, -8, -8, 8, 8, 8, 0xFFD94CFF);
    g_unit_crate  = make_unit_cube(0x4C6AFFFF);
    g_matfp       = malloc_uncached(sizeof(T3DMat4FP));

    kiln_actor_system_init(PROFILES, PROFILE_COUNT, g_pool, ACTOR_POOL_CAP);

    kiln_room_system_init(&g_sys, &g_room, 1, 1,
                         room_load, room_unload, room_spawn, room_draw, NULL,
                         /*owns_clip_world=*/1);

    kiln_physics_init(&g_phys, g_bodies, BODY_CAP);

    /* Spawn 6 crates above the floor in two rows so they stack visibly. */
    for (int i = 0; i < 6; i++) {
        fm_vec3_t pos = {{ (i % 3) * 30.0f - 30.0f,
                          60.0f + (i / 3) * 30.0f,
                          (i / 3) * 30.0f - 15.0f }};
        fm_vec3_t half = {{ 12, 12, 12 }};
        (void)kiln_physics_spawn(&g_phys, KILN_PHYS_DYNAMIC, pos, half, /*mass=*/5.0f);
    }

    /* Player at the centre of the room, on the floor. */
    KilnActorHandle player_h = kiln_actor_spawn(PROFILE_PLAYER,
        (fm_vec3_t){{ 0, 8, 0 }}, 0.0f, NULL);

    KilnScene scene;
    kiln_scene_init(&scene);
    scene.far_z = 600.0f;
    scene.fov_deg = 70.0f;

    int phys_on = 1, bp_on = 0;
    uint16_t last_trace = 0;

    uint32_t frames = 0;
    float fps = 0.0f;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);
        float dt = 1.0f / 60.0f;

        /* Toggles via edges (kiln_input computes edges by diffing against
         * last frame). */
        if (in->edges & KILN_BTN_DL) {
            phys_on = !phys_on;
            kiln_physics_set_enabled(&g_phys, phys_on);
        }
        if (in->edges & KILN_BTN_DR) {
            bp_on = !bp_on;
            kiln_clip_set_broadphase(bp_on);
        }
        if (in->edges & KILN_BTN_DU) { phys_on = 1; kiln_physics_set_enabled(&g_phys, 1); }
        if (in->edges & KILN_BTN_DD) { phys_on = 0; kiln_physics_set_enabled(&g_phys, 0); }

        kiln_actor_update_all(dt);

        /* Player punts the nearest crate in a forward cone on A-edge. */
        KilnActor *player = kiln_actor_resolve(player_h);
        if (player && (in->edges & KILN_BTN_A)) {
            fm_vec3_t ppos = player->xform.pos;
            float pyaw = ((PlayerState *)player->state)->yaw;
            fm_vec3_t pf = {{ fm_cosf(pyaw), 0, -fm_sinf(pyaw) }};
            KilnPhysicsBody *best = NULL;
            float best_d = 80.0f; /* punt radius */
            for (uint16_t i = 0; i < g_phys.count; i++) {
                KilnPhysicsBody *b = &g_phys.bodies[i];
                if (b->type != KILN_PHYS_DYNAMIC) continue;
                fm_vec3_t d = {{ b->pos.v[0] - ppos.v[0],
                                b->pos.v[1] - ppos.v[1],
                                b->pos.v[2] - ppos.v[2] }};
                float dist = fm_vec3_len(&d);
                if (dist > best_d || dist < 1e-3f) continue;
                fm_vec3_norm(&d, &d);
                float dot = d.v[0]*pf.v[0] + d.v[1]*pf.v[1] + d.v[2]*pf.v[2];
                if (dot < 0.5f) continue; /* ~60° half-cone */
                best = b; best_d = dist;
            }
            if (best) {
                fm_vec3_t imp = {{ pf.v[0] * 800.0f, 400.0f, pf.v[2] * 800.0f }};
                kiln_physics_apply_impulse(best, imp);
            }
        }

        /* Step physics. When phys_on is 0 this is a no-op and crates
         * freeze in place. */
        kiln_physics_step(&g_phys, dt);

        /* Room streaming: camera = player pos. The room's brushes are
         * re-installed into the clip world on load/unload — this ROM never
         * calls kiln_clip_set_world. */
        fm_vec3_t cam_target = player ? player->xform.pos
                                      : (fm_vec3_t){{ 0, 0, 0 }};
        kiln_room_system_update(&g_sys, cam_target);

        /* Third-person camera: behind + above the player. */
        scene.cam_target = cam_target;
        scene.cam_pos = (fm_vec3_t){{
            cam_target.v[0], cam_target.v[1] + 80.0f, cam_target.v[2] - 120.0f
        }};
        kiln_scene_update(&scene);

        /* Sample the trace counter after a representative trace this frame
         * — a ground probe from the player. The HUD shows this so the BP
         * toggle's effect is visible. */
        if (player) {
            fm_vec3_t half = {{ 8, 8, 8 }};
            kiln_clip_ground(player->xform.pos, half, half);
            last_trace = kiln_clip_last_trace_brushes();
        }

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        /* ── 3D ───────────────────────────────────────────────────── */
        kiln_frame_begin();
        kiln_scene_begin(&scene);

        /* Draw the room (floor + walls) — world-space verts, no matrix push. */
        kiln_room_draw_all(&g_sys);

        /* Draw each crate body at its current pos via a pushed SRT matrix.
         * The unit cube is scaled by the body's half-extents. */
        for (uint16_t i = 0; i < g_phys.count; i++) {
            KilnPhysicsBody *b = &g_phys.bodies[i];
            float hx = b->maxs.v[0], hy = b->maxs.v[1], hz = b->maxs.v[2];
            T3DMat4 m;
            float scale[3]    = { hx, hy, hz };
            float quat[4]     = { 0.0f, 0.0f, 0.0f, 1.0f };
            float translate[3] = { b->pos.v[0], b->pos.v[1], b->pos.v[2] };
            t3d_mat4_from_srt(&m, scale, quat, translate);
            t3d_mat4_to_fixed(g_matfp, &m);
            t3d_matrix_push(g_matfp);
            draw_cube_mesh(g_unit_crate);
            t3d_matrix_pop(1);
        }

        kiln_actor_draw_all();

        /* ── 2D ───────────────────────────────────────────────────── */
        kiln_gui_begin();
        kiln_gui_panel(8, 8, 220, 100,
                      RGBA32(10, 10, 24, 200), RGBA32(0, 245, 212, 255));
        kiln_gui_text(14, 22, RGBA32(0, 245, 212, 255), "KILN PHYSICS (HL2)");
        kiln_gui_text(14, 34, RGBA32(232, 232, 240, 255), "fps %5.1f", fps);
        kiln_gui_text(14, 46, RGBA32(232, 232, 240, 255),
                     "phys %s   bp %s",
                     phys_on ? "ON " : "OFF",
                     bp_on    ? "ON " : "OFF");
        uint16_t sleeping = 0;
        for (uint16_t i = 0; i < g_phys.count; i++)
            if (g_phys.bodies[i].sleeping) sleeping++;
        kiln_gui_text(14, 58, RGBA32(232, 232, 240, 255),
                     "bodies %u  sleep %u", g_phys.count, sleeping);
        kiln_gui_text(14, 70, RGBA32(232, 232, 240, 255),
                     "traces/step %u", last_trace);
        kiln_gui_text(14, 82, RGBA32(232, 232, 240, 255),
                     "loaded rooms %u",
                     kiln_room_loaded_count(&g_sys));

        kiln_gui_panel(8, SCREEN_H - 28, SCREEN_W - 16, 20,
                      RGBA32(10, 10, 24, 200), RGBA32(139, 92, 246, 255));
        kiln_gui_text(14, SCREEN_H - 18, RGBA32(232, 232, 240, 255),
                     "stick: move  A: punt  DPad-L/R: phys/bp  U/D: phys on/off");
        kiln_gui_end();
        kiln_frame_end();
    }
}