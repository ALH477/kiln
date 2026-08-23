/* SPDX-License-Identifier: MIT
 *
 * kiln_room.h — scene / room streaming. See kiln_actor.h for the pool + handle
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
 * 3D pass with no mesh to occlude against. kiln_room spawns each room's
 * actors only when the room loads, and despawns them when it unloads —
 * triggered through a per-actor `room_id` field on KilnActor.
 *
 * ── Why a callback-per-room rather than a hard-coded mesh type ───────────
 * The engine can't know whether a room's mesh comes from a T3DModel*
 * loaded from DFS, a hand-built T3DVertPacked buffer, both, or neither (an
 * empty room is a valid room). `user_mesh` is therefore a void* that the
 * load callback populates and the unload callback clears; the draw pass
 * itself is the user's responsibility inside kiln_room_draw_all. The engine
 * never dereferences it.
 */
#ifndef KILN_ROOM_H
#define KILN_ROOM_H

#include <libdragon.h>
#include <t3d/t3dmath.h>

#include "kiln_engine.h"
#include "kiln_actor.h"
#include "kiln_clip.h"
#include "kiln_dict.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KILN_ROOM_FLAG_LOADED  (1 << 0)

#define KILN_ROOM_MAX_NEIGHBOURS  8
#define KILN_ROOM_MAX_SPAWNS     16
#define KILN_ROOM_MAX_LOADED     64

/* Cap on the total number of brushes the room system will install into the
 * clip world at once. Sized as max_loaded × brushes/room — 512 covers
 * 64 × 8 which is generous for OoT-room-scale. Override before including the
 * header if a game ships denser rooms. See kiln_room.c's rebuild_clip_world. */
#ifndef KILN_ROOM_MAX_CLIP_BRUSHES
#define KILN_ROOM_MAX_CLIP_BRUSHES 512
#endif

/** A single actor spawn template — the data the user's spawn callback
 *  forwards into kiln_actor_spawn_in_room. Carries a typed key/value dict so
 *  room content can author per-actor spawn args without changing the profile
 *  struct (the idDict analogue from kiln_dict.h). */
typedef struct {
    uint16_t profile_id;
    fm_vec3_t pos;
    float yaw;
    KilnDict dict;    /**< spawn args; read by the profile's init callback     */
} KilnRoomSpawn;

/** One room. The user fills one of these per logical area in KilnSceneArea,
 *  then hands the array to kiln_room_system_init. The engine never reads
 *  `user_mesh` — it only sets it to NULL on init, calls on_load to populate,
 *  and on_unload to clear. `brushes` is the collision analogue: on_load
 *  populates it with the room's KilnBrush array (caller-owned; on_unload
 *  frees it). The engine copies each loaded room's brushes into one
 *  module-static world buffer and installs it via kiln_clip_set_world, so a
 *  ROM using kiln_room never calls kiln_clip_set_world itself. A room with no
 *  collision leaves brushes == NULL and brush_count == 0. */
typedef struct KilnRoom {
    uint8_t id;       /* its own index in KilnSceneArea.rooms[] */
    uint8_t flags;
    uint8_t neighbour_count;
    uint8_t neighbours[KILN_ROOM_MAX_NEIGHBOURS];

    fm_vec3_t aabb_min;
    fm_vec3_t aabb_max;

    uint8_t spawn_count;
    KilnRoomSpawn spawns[KILN_ROOM_MAX_SPAWNS];

    void *user_mesh;  /* set by on_load, cleared by on_unload */
    KilnBrush *brushes; /* set by on_load (or pre-filled at construction),  */
    uint16_t  brush_count; /*   cleared by on_unload. Caller-owned.        */
} KilnRoom;

/** Streaming state. Embedded by value into KilnSceneArea so the area and its
 *  subsystem move together. */
typedef struct {
    KilnRoom *rooms;
    uint16_t room_count;
    uint16_t max_loaded;

    uint8_t loaded_slots[KILN_ROOM_MAX_LOADED];
    uint16_t loaded_count;

    int16_t active_room;
    fm_vec3_t last_camera_pos;

    void *user_ctx;
} KilnRoomSystem;

typedef void (*KilnRoomLoadFn)  (KilnRoom *room, void *user);
typedef void (*KilnRoomUnloadFn)(KilnRoom *room, void *user);
/** Forward into kiln_actor_spawn_with_args(..., &spawn->dict, room->id). */
typedef void (*KilnRoomSpawnFn) (KilnRoom *room, const KilnRoomSpawn *spawn,
                                void *user);
/** Draw the room's geometry. Called once per loaded room per frame from
 *  kiln_room_draw_all. The engine never dereferences user_mesh — the draw
 *  callback does, after which it can free or refresh the buffer as it
 *  pleases. */
typedef void (*KilnRoomDrawFn)  (KilnRoom *room, void *user);

/** Bind the room array and the four callbacks. Asserts that room_count and
 *  max_loaded fit the module's fixed-size tables. The user passes one
 *  `user_ctx` pointer that round-trips through every callback — the same
 *  trick as the actor system's profile table, so callbacks can avoid
 *  globals. `draw_fn` may be NULL if the room has no per-frame geometry.
 *
 *  `owns_clip_world`: when non-zero (the normal case), the room system
 *  concatenates each loaded room's `brushes` into one module-static buffer
 *  and installs it via kiln_clip_set_world after every load and unload — so
 *  the ROM never calls kiln_clip_set_world itself. Pass 0 only if the ROM
 *  wants to mix kiln_room with a hand-managed clip world; doing both is
 *  exclusive, the next room update will clobber a manual install. */
void kiln_room_system_init(KilnRoomSystem *sys, KilnRoom *rooms, uint16_t room_count,
                          uint16_t max_loaded,
                          KilnRoomLoadFn load_fn, KilnRoomUnloadFn unload_fn,
                          KilnRoomSpawnFn spawn_fn, KilnRoomDrawFn draw_fn,
                          void *user_ctx, int owns_clip_world);

/** Per-frame update. Three strict-order passes:
 *   (A) compute desired set from camera-AABB-intersect + neighbours
 *   (B) unload phase — clear LOADED flag, despawn the room's actors, then
 *       call on_unload, then compact out of loaded_slots
 *   (C) load phase — call on_load, set LOADED flag, append to loaded_slots,
 *       then call on_spawn for each template
 *  The despawn-before-free and load-before-spawn orderings are the contract;
 *  see the .c file for why each matters. */
void kiln_room_system_update(KilnRoomSystem *sys, fm_vec3_t camera_pos);

/** NULL if the room is not currently loaded. */
KilnRoom *kiln_room_loaded(KilnRoomSystem *sys, uint8_t room_id);

uint16_t kiln_room_loaded_count(KilnRoomSystem *sys);

/** The room the camera is currently inside (point-in-AABB), or NULL if
 *  between rooms this frame. Use this for sound / music routing. */
KilnRoom *kiln_room_current(KilnRoomSystem *sys);

/** Walk the loaded set in load order. */
KilnRoom *kiln_room_first_loaded(KilnRoomSystem *sys);
KilnRoom *kiln_room_next_loaded(KilnRoomSystem *sys, KilnRoom *cur);

/** Iterate the loaded rooms in draw order and let each draw its own
 *  user_mesh. Call between kiln_scene_begin and kiln_actor_draw_all. The
 *  engine never dereferences user_mesh — the user's load_fn is responsible
 *  for populating it with whatever drawing primitive the room needs. A
 *  typical hand-built room does `t3d_vert_load` + `t3d_tri_draw` loop +
 *  `t3d_tri_sync` here. */
void kiln_room_draw_all(KilnRoomSystem *sys);

#ifdef __cplusplus
}
#endif

#endif /* KILN_ROOM_H */