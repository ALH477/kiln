/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_room.c — see m64_room.h for the model. The implementation follows the
 * same module-private-globals + caller-owned-pool convention as m64_actor.
 */

#include "m64_room.h"

#include <stddef.h>
#include <string.h>

#define CAMERA_AABB_HALF 20.0f

/* Single active M64RoomSystem at a time. The engine has no multi-scene
 * concept; if a future game wants two systems, this module grows a
 * "select active system" entry point. */
static M64RoomSystem *g_sys;

static M64RoomLoadFn   g_on_load;
static M64RoomUnloadFn g_on_unload;
static M64RoomSpawnFn  g_on_spawn;
static M64RoomDrawFn   g_on_draw;

/* ── helpers ─────────────────────────────────────────────────────────── */

static inline int aabb_contains(fm_vec3_t min, fm_vec3_t max, fm_vec3_t p)
{
    return p.v[0] >= min.v[0] && p.v[0] <= max.v[0]
        && p.v[1] >= min.v[1] && p.v[1] <= max.v[1]
        && p.v[2] >= min.v[2] && p.v[2] <= max.v[2];
}

static inline int aabb_intersects(fm_vec3_t amin, fm_vec3_t amax,
                                  fm_vec3_t bmin, fm_vec3_t bmax)
{
    return amin.v[0] <= bmax.v[0] && amax.v[0] >= bmin.v[0]
        && amin.v[1] <= bmax.v[1] && amax.v[1] >= bmin.v[1]
        && amin.v[2] <= bmax.v[2] && amax.v[2] >= bmin.v[2];
}

static int is_loaded(uint8_t id)
{
    for (uint16_t i = 0; i < g_sys->loaded_count; i++)
        if (g_sys->loaded_slots[i] == id) return 1;
    return 0;
}

static void append_loaded(uint8_t id)
{
    assertf(g_sys->loaded_count < g_sys->max_loaded,
            "m64_room: loaded_count %u would exceed max_loaded %u",
            g_sys->loaded_count, g_sys->max_loaded);
    assertf(g_sys->loaded_count < M64_ROOM_MAX_LOADED,
            "m64_room: M64_ROOM_MAX_LOADED %u exceeded",
            M64_ROOM_MAX_LOADED);
    g_sys->loaded_slots[g_sys->loaded_count++] = id;
}

static void remove_loaded(uint8_t id)
{
    for (uint16_t i = 0; i < g_sys->loaded_count; i++) {
        if (g_sys->loaded_slots[i] == id) {
            for (uint16_t j = i; j + 1 < g_sys->loaded_count; j++)
                g_sys->loaded_slots[j] = g_sys->loaded_slots[j + 1];
            g_sys->loaded_count--;
            return;
        }
    }
}

/* Despawn every actor whose `room_id` matches `r->id`. Walks every
 * category; cost is O(actors) and only fires on a transition frame, which
 * is exactly when you'd want a full scan to happen anyway. The walk uses
 * m64_actor_first/next, both of which read the intrusive category list and
 * capture nothing per iteration — so a despawn that despawns another of
 * the same room (cascading) is safe here. */
static void despawn_room_actors(M64Room *r)
{
    for (uint8_t c = 0; c < M64_ACTOR_CATEGORY_COUNT; c++) {
        M64Actor *cur = m64_actor_first(c);
        while (cur) {
            M64Actor *next = m64_actor_next(cur);
            if (cur->room_id == r->id) {
                m64_actor_despawn(m64_actor_handle_of(cur));
            }
            cur = next;
        }
    }
}

/* ── public API ──────────────────────────────────────────────────────── */

void m64_room_system_init(M64RoomSystem *sys, M64Room *rooms, uint16_t room_count,
                          uint16_t max_loaded,
                          M64RoomLoadFn load_fn, M64RoomUnloadFn unload_fn,
                          M64RoomSpawnFn spawn_fn, M64RoomDrawFn draw_fn,
                          void *user_ctx)
{
    assertf(sys != NULL, "m64_room: sys is NULL");
    assertf(rooms != NULL || room_count == 0, "m64_room: rooms NULL with room_count %u", room_count);
    assertf(max_loaded <= M64_ROOM_MAX_LOADED,
            "m64_room: max_loaded %u exceeds M64_ROOM_MAX_LOADED %u",
            max_loaded, M64_ROOM_MAX_LOADED);
    assertf(load_fn && unload_fn && spawn_fn,
            "m64_room: load/unload/spawn callbacks must all be non-NULL");

    sys->rooms = rooms;
    sys->room_count = room_count;
    sys->max_loaded = max_loaded;
    sys->loaded_count = 0;
    sys->active_room = -1;
    sys->user_ctx = user_ctx;

    /* Tag every room with its own array index so callbacks can ignore the
     * redundant `id` field the user fills at construction. Idempotent: a
     * caller-supplied `id` is overwritten so a hand-written array of
     * literal M64Room values with `[N]` initialisers still works. */
    for (uint16_t i = 0; i < room_count; i++) {
        rooms[i].id = (uint8_t)i;
        rooms[i].flags = 0;
        rooms[i].user_mesh = NULL;
    }

    g_sys = sys;
    g_on_load = load_fn;
    g_on_unload = unload_fn;
    g_on_spawn = spawn_fn;
    g_on_draw = draw_fn; /* may be NULL */
}

void m64_room_system_update(M64RoomSystem *sys, fm_vec3_t camera_pos)
{
    g_sys = sys; /* cheap pointer rotation, mirrors m64_actor's no-system-pattern */

    /* Camera AABB — a small box around the camera point. +20 on each axis is
     * generous for the demo; a real game would pass it in from the
     * M64Scene's near_z / player radius. */
    fm_vec3_t cam_min = {{ camera_pos.v[0] - CAMERA_AABB_HALF,
                           camera_pos.v[1] - CAMERA_AABB_HALF,
                           camera_pos.v[2] - CAMERA_AABB_HALF }};
    fm_vec3_t cam_max = {{ camera_pos.v[0] + CAMERA_AABB_HALF,
                           camera_pos.v[1] + CAMERA_AABB_HALF,
                           camera_pos.v[2] + CAMERA_AABB_HALF }};

    /* Pass A: compute desired set. */
    uint8_t desired[M64_ROOM_MAX_LOADED];
    uint16_t desired_count = 0;

    for (uint16_t i = 0; i < sys->room_count; i++) {
        M64Room *r = &sys->rooms[i];

        int candidate =
            aabb_intersects(cam_min, cam_max, r->aabb_min, r->aabb_max) ||
            /* A loaded room's neighbours are also candidates — this is the
             * pre-load that hides seam pop-in. A neighbour that shares a
             * border with the current room will get loaded the moment the
             * camera touches the border, not the moment the camera enters
             * it. */
            (r->flags & M64_ROOM_FLAG_LOADED);

        /* If this room is currently loaded, OR a loaded room lists it as
         * a neighbour, keep it in the desired set. */
        if (!candidate) {
            for (uint8_t j = 0; j < r->neighbour_count; j++) {
                uint8_t n = r->neighbours[j];
                if (n < sys->room_count && is_loaded(n)) {
                    candidate = 1;
                    break;
                }
            }
        }

        if (candidate && desired_count < M64_ROOM_MAX_LOADED) {
            desired[desired_count++] = r->id;
        }
    }

    /* Pass B: unload phase. Walk `desired` vs `loaded_slots`: anything in
     * loaded but not desired gets torn down. */
    for (uint16_t i = 0; i < sys->loaded_count; ) {
        uint8_t id = sys->loaded_slots[i];
        int keep = 0;
        for (uint16_t j = 0; j < desired_count; j++)
            if (desired[j] == id) { keep = 1; break; }
        if (keep) { i++; continue; }

        M64Room *r = &sys->rooms[id];
        /* Clear LOADED first so anything that re-enters m64_room_loaded
         * during teardown sees the new state. */
        r->flags &= ~M64_ROOM_FLAG_LOADED;
        /* Despawn BEFORE freeing the room's mesh — the actor draw code
         * references user_mesh (via the user's draw callback chain), and a
         * free here would leave it dangling for one frame. */
        despawn_room_actors(r);
        g_on_unload(r, sys->user_ctx);
        remove_loaded(id);
        /* don't increment i — the slot shifted in */
    }

    /* Pass C: load phase. Walk `desired` and load anything not yet in
     * loaded_slots. */
    for (uint16_t i = 0; i < desired_count; i++) {
        uint8_t id = desired[i];
        if (is_loaded(id)) continue;
        M64Room *r = &sys->rooms[id];

        g_on_load(r, sys->user_ctx);
        r->flags |= M64_ROOM_FLAG_LOADED;
        append_loaded(id);

        /* Spawn AFTER load+append so the actor draw pass (later in the
         * frame) sees the room as loaded and the meshes the spawn
         * positions reference exist. */
        for (uint8_t j = 0; j < r->spawn_count; j++) {
            M64RoomSpawn *s = &r->spawns[j];
            g_on_spawn(r, s->profile_id, s->pos, s->yaw, sys->user_ctx);
        }
    }

    /* active_room: the loaded room whose AABB contains the camera point.
     * If multiple claim it (border), the lower id wins; that's deterministic
     * and matches OoT's behaviour for sound/music routing. */
    sys->active_room = -1;
    for (uint16_t i = 0; i < sys->loaded_count; i++) {
        M64Room *r = &sys->rooms[sys->loaded_slots[i]];
        if (aabb_contains(r->aabb_min, r->aabb_max, camera_pos)) {
            if (sys->active_room < 0 || r->id < (uint8_t)sys->active_room)
                sys->active_room = r->id;
        }
    }

    sys->last_camera_pos = camera_pos;
}

M64Room *m64_room_loaded(M64RoomSystem *sys, uint8_t room_id)
{
    if (room_id >= sys->room_count) return NULL;
    return is_loaded(room_id) ? &sys->rooms[room_id] : NULL;
}

uint16_t m64_room_loaded_count(M64RoomSystem *sys)
{
    return sys->loaded_count;
}

M64Room *m64_room_current(M64RoomSystem *sys)
{
    if (sys->active_room < 0) return NULL;
    return &sys->rooms[sys->active_room];
}

M64Room *m64_room_first_loaded(M64RoomSystem *sys)
{
    if (sys->loaded_count == 0) return NULL;
    return &sys->rooms[sys->loaded_slots[0]];
}

M64Room *m64_room_next_loaded(M64RoomSystem *sys, M64Room *cur)
{
    if (!cur) return NULL;
    int next_idx = -1;
    for (uint16_t i = 0; i + 1 < sys->loaded_count; i++) {
        if (&sys->rooms[sys->loaded_slots[i]] == cur) {
            next_idx = sys->loaded_slots[i + 1];
            break;
        }
    }
    return next_idx < 0 ? NULL : &sys->rooms[next_idx];
}

void m64_room_draw_all(M64RoomSystem *sys)
{
    if (!g_on_draw) return;
    for (uint16_t i = 0; i < sys->loaded_count; i++) {
        M64Room *r = &sys->rooms[sys->loaded_slots[i]];
        g_on_draw(r, sys->user_ctx);
    }
}