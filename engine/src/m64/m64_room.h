/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_room.h — scene / room streaming. See m64_actor.h for the pool + handle
 * conventions this module follows.
 *
 * ── Why a scene/room split instead of one big world model ───────────────
 * One monolithic world does not fit on a 4 MB RDRAM. The split exists so that
 * each frame only the rooms near the camera need their meshes, actors and
 * sound resident. On N64 this is not an optimisation, it is the reason the
 * world can be bigger than memory — every loaded byte that isn't this room's
 * is a byte the rest of the game has lost.
 *
 * ── Why streaming is AABB-overlap, not a radius ─────────────────────────
 * A radius around the camera would also pull the two diagonal rooms at every
 * corner, costing two rooms' worth of RAM for one room's worth of vision.
 * AABB-overlap with the camera's AABB produces the natural cross-shaped
 * loaded set OoT uses, which keeps the count predictable. A room that shares
 * a border with a loaded room is also marked as a candidate via its
 * `neighbours[]` table — this is the seam the brief describes as "rooms
 * loaded and unloaded as the player moves".
 *
 * ── Why spawn templates are deferred to load time ───────────────────────
 * An actor that exists in RAM but is not in any loaded room is wasted: it
 * updates every frame, costs pool capacity, and its draw callback runs in a
 * 3D pass with no mesh to occlude against. m64_room spawns each room's
 * actors only when the room loads, and despawns them when it unloads —
 * triggered through a per-actor `room_id` field on M64Actor.
 *
 * ── Why a callback-per-room rather than a hard-coded mesh type ───────────
 * The engine can't know whether a room's mesh comes from a T3DModel*
 * loaded from DFS, a hand-built T3DVertPacked buffer, both, or neither (an
 * empty room is a valid room). `user_mesh` is therefore a void* that the
 * load callback populates and the unload callback clears; the draw pass
 * itself is the user's responsibility inside m64_room_draw_all. The engine
 * never dereferences it.
 */
#ifndef M64_ROOM_H
#define M64_ROOM_H

#include <libdragon.h>
#include <t3d/t3dmath.h>

#include "m64_engine.h"
#include "m64_actor.h"

#ifdef __cplusplus
extern "C" {
#endif

#define M64_ROOM_FLAG_LOADED  (1 << 0)

#define M64_ROOM_MAX_NEIGHBOURS  8
#define M64_ROOM_MAX_SPAWNS     16
#define M64_ROOM_MAX_LOADED     64

/** A single actor spawn template — the data the user's spawn callback
 *  forwards into m64_actor_spawn_in_room. */
typedef struct {
    uint16_t profile_id;
    fm_vec3_t pos;
    float yaw;
} M64RoomSpawn;

/** One room. The user fills one of these per logical area in M64SceneArea,
 *  then hands the array to m64_room_system_init. The engine never reads
 *  `user_mesh` — it only sets it to NULL on init, calls on_load to populate,
 *  and on_unload to clear. */
typedef struct M64Room {
    uint8_t id;       /* its own index in M64SceneArea.rooms[] */
    uint8_t flags;
    uint8_t neighbour_count;
    uint8_t neighbours[M64_ROOM_MAX_NEIGHBOURS];

    fm_vec3_t aabb_min;
    fm_vec3_t aabb_max;

    uint8_t spawn_count;
    M64RoomSpawn spawns[M64_ROOM_MAX_SPAWNS];

    void *user_mesh;  /* set by on_load, cleared by on_unload */
} M64Room;

/** Streaming state. Embedded by value into M64SceneArea so the area and its
 *  subsystem move together. */
typedef struct {
    M64Room *rooms;
    uint16_t room_count;
    uint16_t max_loaded;

    uint8_t loaded_slots[M64_ROOM_MAX_LOADED];
    uint16_t loaded_count;

    int16_t active_room;
    fm_vec3_t last_camera_pos;

    void *user_ctx;
} M64RoomSystem;

typedef void (*M64RoomLoadFn)  (M64Room *room, void *user);
typedef void (*M64RoomUnloadFn)(M64Room *room, void *user);
/** Forward into m64_actor_spawn_in_room(..., room->id). */
typedef void (*M64RoomSpawnFn) (M64Room *room, uint16_t profile_id,
                                fm_vec3_t pos, float yaw, void *user);
/** Draw the room's geometry. Called once per loaded room per frame from
 *  m64_room_draw_all. The engine never dereferences user_mesh — the draw
 *  callback does, after which it can free or refresh the buffer as it
 *  pleases. */
typedef void (*M64RoomDrawFn)  (M64Room *room, void *user);

/** Bind the room array and the four callbacks. Asserts that room_count and
 *  max_loaded fit the module's fixed-size tables. The user passes one
 *  `user_ctx` pointer that round-trips through every callback — the same
 *  trick as the actor system's profile table, so callbacks can avoid
 *  globals. `draw_fn` may be NULL if the room has no per-frame geometry. */
void m64_room_system_init(M64RoomSystem *sys, M64Room *rooms, uint16_t room_count,
                          uint16_t max_loaded,
                          M64RoomLoadFn load_fn, M64RoomUnloadFn unload_fn,
                          M64RoomSpawnFn spawn_fn, M64RoomDrawFn draw_fn,
                          void *user_ctx);

/** Per-frame update. Three strict-order passes:
 *   (A) compute desired set from camera-AABB-intersect + neighbours
 *   (B) unload phase — clear LOADED flag, despawn the room's actors, then
 *       call on_unload, then compact out of loaded_slots
 *   (C) load phase — call on_load, set LOADED flag, append to loaded_slots,
 *       then call on_spawn for each template
 *  The despawn-before-free and load-before-spawn orderings are the contract;
 *  see the .c file for why each matters. */
void m64_room_system_update(M64RoomSystem *sys, fm_vec3_t camera_pos);

/** NULL if the room is not currently loaded. */
M64Room *m64_room_loaded(M64RoomSystem *sys, uint8_t room_id);

uint16_t m64_room_loaded_count(M64RoomSystem *sys);

/** The room the camera is currently inside (point-in-AABB), or NULL if
 *  between rooms this frame. Use this for sound / music routing. */
M64Room *m64_room_current(M64RoomSystem *sys);

/** Walk the loaded set in load order. */
M64Room *m64_room_first_loaded(M64RoomSystem *sys);
M64Room *m64_room_next_loaded(M64RoomSystem *sys, M64Room *cur);

/** Iterate the loaded rooms in draw order and let each draw its own
 *  user_mesh. Call between m64_scene_begin and m64_actor_draw_all. The
 *  engine never dereferences user_mesh — the user's load_fn is responsible
 *  for populating it with whatever drawing primitive the room needs. A
 *  typical hand-built room does `t3d_vert_load` + `t3d_tri_draw` loop +
 *  `t3d_tri_sync` here. */
void m64_room_draw_all(M64RoomSystem *sys);

#ifdef __cplusplus
}
#endif

#endif /* M64_ROOM_H */