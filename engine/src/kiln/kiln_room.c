/* SPDX-License-Identifier: MIT
 *
 * kiln_room.c — see kiln_room.h for the model. The implementation follows the
 * same module-private-globals + caller-owned-pool convention as kiln_actor.
 */

#include "kiln_room.h"
#include "kiln_clip.h"

#include <stddef.h>
#include <string.h>

#define CAMERA_AABB_HALF 20.0f

/* Single active KilnRoomSystem at a time. The engine has no multi-scene
 * concept; if a future game wants two systems, this module grows a
 * "select active system" entry point. */
static KilnRoomSystem *g_sys;

static KilnRoomLoadFn   g_on_load;
static KilnRoomUnloadFn g_on_unload;
static KilnRoomSpawnFn  g_on_spawn;
static KilnRoomDrawFn   g_on_draw;
static int             g_owns_clip_world;

/* Concatenated brush buffer for the clip world. After every load and unload
 * phase, rebuild_clip_world() walks the loaded rooms and copies their
 * `brushes` arrays into this buffer, then hands it to kiln_clip_set_world.
 * The buffer is module-static so the pointer kiln_clip borrows stays stable
 * across transitions — kiln_clip_set_world's contract requires the array
 * outlive every subsequent trace, and a static buffer is the simplest way
 * to guarantee that without the caller managing a merged buffer per ROM. */
static KilnBrush g_world_brushes[KILN_ROOM_MAX_CLIP_BRUSHES];
static uint16_t g_world_brush_count;

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
            "kiln_room: loaded_count %u would exceed max_loaded %u",
            g_sys->loaded_count, g_sys->max_loaded);
    assertf(g_sys->loaded_count < KILN_ROOM_MAX_LOADED,
            "kiln_room: KILN_ROOM_MAX_LOADED %u exceeded",
            KILN_ROOM_MAX_LOADED);
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
 * kiln_actor_first/next, both of which read the intrusive category list and
 * capture nothing per iteration — so a despawn that despawns another of
 * the same room (cascading) is safe here. */
static void despawn_room_actors(KilnRoom *r)
{
    for (uint8_t c = 0; c < KILN_ACTOR_CATEGORY_COUNT; c++) {
        KilnActor *cur = kiln_actor_first(c);
        while (cur) {
            KilnActor *next = kiln_actor_next(cur);
            if (cur->room_id == r->id) {
                kiln_actor_despawn(kiln_actor_handle_of(cur));
            }
            cur = next;
        }
    }
}

/* Concatenate every loaded room's `brushes` into g_world_brushes and install
 * it via kiln_clip_set_world. Called once after the unload phase and once
 * after the load phase — two rebuilds per transition frame is nothing next
 * to the cost of the load/unload work itself, and the split is what lets
 * g_on_unload query collision against the pre-transition world and g_on_spawn
 * query it against the post-transition world. The cap is asserted: if a
 * game ever exceeds it, bump KILN_ROOM_MAX_CLIP_BRUSHES or set
 * owns_clip_world = 0 at init and manage the clip world by hand. */
static void rebuild_clip_world(void)
{
    if (!g_owns_clip_world) return;

    g_world_brush_count = 0;
    for (uint16_t i = 0; i < g_sys->loaded_count; i++) {
        KilnRoom *r = &g_sys->rooms[g_sys->loaded_slots[i]];
        if (r->brush_count == 0 || r->brushes == NULL) continue;
        assertf(g_world_brush_count + r->brush_count <= KILN_ROOM_MAX_CLIP_BRUSHES,
                "kiln_room: clip brush cap %u exceeded (loaded rooms sum to >%u "
                "brushes) — raise KILN_ROOM_MAX_CLIP_BRUSHES or pass "
                "owns_clip_world=0 to kiln_room_system_init",
                KILN_ROOM_MAX_CLIP_BRUSHES,
                g_world_brush_count + r->brush_count);
        memcpy(&g_world_brushes[g_world_brush_count], r->brushes,
               sizeof(KilnBrush) * r->brush_count);
        g_world_brush_count += r->brush_count;
    }
    kiln_clip_set_world(g_world_brushes, g_world_brush_count);
}

/* ── public API ──────────────────────────────────────────────────────── */

void kiln_room_system_init(KilnRoomSystem *sys, KilnRoom *rooms, uint16_t room_count,
                          uint16_t max_loaded,
                          KilnRoomLoadFn load_fn, KilnRoomUnloadFn unload_fn,
                          KilnRoomSpawnFn spawn_fn, KilnRoomDrawFn draw_fn,
                          void *user_ctx, int owns_clip_world)
{
    assertf(sys != NULL, "kiln_room: sys is NULL");
    assertf(rooms != NULL || room_count == 0, "kiln_room: rooms NULL with room_count %u", room_count);
    assertf(max_loaded <= KILN_ROOM_MAX_LOADED,
            "kiln_room: max_loaded %u exceeds KILN_ROOM_MAX_LOADED %u",
            max_loaded, KILN_ROOM_MAX_LOADED);
    assertf(load_fn && unload_fn && spawn_fn,
            "kiln_room: load/unload/spawn callbacks must all be non-NULL");

    sys->rooms = rooms;
    sys->room_count = room_count;
    sys->max_loaded = max_loaded;
    sys->loaded_count = 0;
    sys->active_room = -1;
    sys->user_ctx = user_ctx;

    /* Tag every room with its own array index so callbacks can ignore the
     * redundant `id` field the user fills at construction. Idempotent: a
     * caller-supplied `id` is overwritten so a hand-written array of
     * literal KilnRoom values with `[N]` initialisers still works. */
    for (uint16_t i = 0; i < room_count; i++) {
        rooms[i].id = (uint8_t)i;
        rooms[i].flags = 0;
        rooms[i].user_mesh = NULL;
        /* brushes/brush_count are not zeroed here: a ROM may pre-fill them
         * at construction (static brush arrays for hand-authored rooms).
         * on_load is the canonical place to populate them for streamed
         * content; on_unload must clear what on_load set. */
    }

    g_sys = sys;
    g_on_load = load_fn;
    g_on_unload = unload_fn;
    g_on_spawn = spawn_fn;
    g_on_draw = draw_fn; /* may be NULL */
    g_owns_clip_world = owns_clip_world ? 1 : 0;
    g_world_brush_count = 0;
    if (g_owns_clip_world) {
        kiln_clip_set_world(g_world_brushes, 0);
    }
}

void kiln_room_system_update(KilnRoomSystem *sys, fm_vec3_t camera_pos)
{
    g_sys = sys; /* cheap pointer rotation, mirrors kiln_actor's no-system-pattern */

    /* Camera AABB — a small box around the camera point. +20 on each axis is
     * generous for the demo; a real game would pass it in from the
     * KilnScene's near_z / player radius. */
    fm_vec3_t cam_min = {{ camera_pos.v[0] - CAMERA_AABB_HALF,
                           camera_pos.v[1] - CAMERA_AABB_HALF,
                           camera_pos.v[2] - CAMERA_AABB_HALF }};
    fm_vec3_t cam_max = {{ camera_pos.v[0] + CAMERA_AABB_HALF,
                           camera_pos.v[1] + CAMERA_AABB_HALF,
                           camera_pos.v[2] + CAMERA_AABB_HALF }};

    /* Pass A: compute the desired set.
     *
     * The rooms the camera box touches — the one containing the camera point
     * first — then THEIR neighbours, stopping at max_loaded.
     *
     * This used to be "touches the camera, OR is already loaded, OR neighbours
     * any loaded room". That is a closure, not a set: a loaded room kept itself
     * loaded forever, and every frame added one more ring of neighbours until
     * the set tried to exceed max_loaded and append_loaded asserted. On a 2x2
     * grid with max_loaded 3 that is the third frame after boot. examples/
     * rooms-demo walled its player into room A as well, so no room was ever
     * seen to unload, because none ever could. */
    uint8_t desired[KILN_ROOM_MAX_LOADED];
    uint16_t desired_count = 0;
    const uint16_t cap = sys->max_loaded < KILN_ROOM_MAX_LOADED
                       ? sys->max_loaded : KILN_ROOM_MAX_LOADED;

    for (int pass = 0; pass < 2; pass++) {
        for (uint16_t i = 0; i < sys->room_count && desired_count < cap; i++) {
            KilnRoom *r = &sys->rooms[i];
            const int hit = pass == 0
                ? aabb_contains(r->aabb_min, r->aabb_max, camera_pos)
                : aabb_intersects(cam_min, cam_max, r->aabb_min, r->aabb_max);
            if (!hit) continue;
            int dup = 0;
            for (uint16_t j = 0; j < desired_count; j++)
                if (desired[j] == r->id) { dup = 1; break; }
            if (!dup) desired[desired_count++] = r->id;
        }
    }

    /* Neighbours of the rooms the camera is in — not of whatever happens to
     * be loaded — so the set is a function of where the camera is. */
    const uint16_t seeds = desired_count;
    for (uint16_t k = 0; k < seeds; k++) {
        const KilnRoom *r = &sys->rooms[desired[k]];
        for (uint8_t j = 0; j < r->neighbour_count && desired_count < cap; j++) {
            const uint8_t n = r->neighbours[j];
            if (n >= sys->room_count) continue;
            int dup = 0;
            for (uint16_t d = 0; d < desired_count; d++)
                if (desired[d] == n) { dup = 1; break; }
            if (!dup) desired[desired_count++] = n;
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

        KilnRoom *r = &sys->rooms[id];
        /* Clear LOADED first so anything that re-enters kiln_room_loaded
         * during teardown sees the new state. */
        r->flags &= ~KILN_ROOM_FLAG_LOADED;
        /* Despawn BEFORE freeing the room's mesh — the actor draw code
         * references user_mesh (via the user's draw callback chain), and a
         * free here would leave it dangling for one frame. */
        despawn_room_actors(r);
        /* on_unload runs against the pre-transition clip world — the room's
         * brushes are still installed, so anything on_unload queries sees
         * the right state. on_unload is also where the user frees
         * room->brushes if on_load malloc'd them. */
        g_on_unload(r, sys->user_ctx);
        remove_loaded(id);
        /* don't increment i — the slot shifted in */
    }
    /* Rebuild the clip world after the unload phase so the just-unloaded
     * rooms' brushes are gone before the load phase runs. on_spawn in the
     * load phase then sees the post-unload world. */
    rebuild_clip_world();

    /* Pass C: load phase. Walk `desired` and load anything not yet in
     * loaded_slots. */
    for (uint16_t i = 0; i < desired_count; i++) {
        uint8_t id = desired[i];
        if (is_loaded(id)) continue;
        KilnRoom *r = &sys->rooms[id];

        g_on_load(r, sys->user_ctx);
        /* Rebuild the clip world between g_on_load and g_on_spawn so the
         * newly-loaded room's brushes are visible to the spawn callback —
         * a spawn that does a trace (e.g. drop-to-ground) sees the new
         * world. Rebuild per load is cheap (the buffer is small) and
         * simpler than tracking per-room deltas. */
        rebuild_clip_world();
        r->flags |= KILN_ROOM_FLAG_LOADED;
        append_loaded(id);

        /* Spawn AFTER load+append so the actor draw pass (later in the
         * frame) sees the room as loaded and the meshes the spawn
         * positions reference exist. */
        for (uint8_t j = 0; j < r->spawn_count; j++) {
            g_on_spawn(r, &r->spawns[j], sys->user_ctx);
        }
    }

    /* active_room: the loaded room whose AABB contains the camera point.
     * If multiple claim it (border), the lower id wins; that's deterministic
     * and matches OoT's behaviour for sound/music routing. */
    sys->active_room = -1;
    for (uint16_t i = 0; i < sys->loaded_count; i++) {
        KilnRoom *r = &sys->rooms[sys->loaded_slots[i]];
        if (aabb_contains(r->aabb_min, r->aabb_max, camera_pos)) {
            if (sys->active_room < 0 || r->id < (uint8_t)sys->active_room)
                sys->active_room = r->id;
        }
    }

    sys->last_camera_pos = camera_pos;
}

KilnRoom *kiln_room_loaded(KilnRoomSystem *sys, uint8_t room_id)
{
    if (room_id >= sys->room_count) return NULL;
    return is_loaded(room_id) ? &sys->rooms[room_id] : NULL;
}

uint16_t kiln_room_loaded_count(KilnRoomSystem *sys)
{
    return sys->loaded_count;
}

KilnRoom *kiln_room_current(KilnRoomSystem *sys)
{
    if (sys->active_room < 0) return NULL;
    return &sys->rooms[sys->active_room];
}

KilnRoom *kiln_room_first_loaded(KilnRoomSystem *sys)
{
    if (sys->loaded_count == 0) return NULL;
    return &sys->rooms[sys->loaded_slots[0]];
}

KilnRoom *kiln_room_next_loaded(KilnRoomSystem *sys, KilnRoom *cur)
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

void kiln_room_draw_all(KilnRoomSystem *sys)
{
    if (!g_on_draw) return;
    for (uint16_t i = 0; i < sys->loaded_count; i++) {
        KilnRoom *r = &sys->rooms[sys->loaded_slots[i]];
        g_on_draw(r, sys->user_ctx);
    }
}