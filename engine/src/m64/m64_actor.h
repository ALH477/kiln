/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_actor.h — the actor system. Every live thing in the world (player,
 * enemies, NPCs, props, items, bosses, doors, chests) is an M64Actor with a
 * shared header plus a small inline block of per-type state, registered
 * through a profile table. Modelled on Ocarina of Time's Actor/ActorProfile
 * split — see CLAUDE.md's Phase B notes for what was and wasn't carried over.
 *
 * ── Why a flat pool, not malloc per actor ─────────────────────────────────
 * The pool is caller-supplied fixed-size storage (`m64_actor_system_init`
 * takes the array, not a capacity to allocate). One bounded allocation
 * failure at boot beats a heap that can fragment mid-level. It also makes the
 * pool one contiguous, cache-friendly block on a VR4300 with 8 KB of D-cache,
 * the same reasoning streamdb-embedded's arena uses.
 *
 * ── Why per-type state is inline, not a per-type malloc ───────────────────
 * Every actor's state lives in a fixed `M64_ACTOR_STATE_MAX`-byte block
 * inside the shared struct, sized to the largest actor type in the game
 * (override the macro before including this header if 64 bytes is too
 * small). This is OoT's "one instance struct per active actor, sized to the
 * overlay's max" idea without the overlay/segment paging machinery — see
 * CLAUDE.md for why paging is deliberately not here yet.
 *
 * ── Category lists, not one flat walk ─────────────────────────────────────
 * Actors are linked (by pool index, intrusively) into one list per
 * M64ActorCategory, so `m64_actor_update_all` / `m64_actor_draw_all` visit
 * every actor in a FIXED category order — player before enemies, enemies
 * before props, etc. That ordering is also how OoT gets predictable draw
 * order and cheap category-scoped queries (m64_actor_first/next) for free.
 *
 * ── Handles, not raw pointers ──────────────────────────────────────────────
 * m64_actor_spawn returns an M64ActorHandle (pool index + generation
 * counter packed into one uint32_t), not an M64Actor*. A pointer an actor's
 * update function cached across a despawn/respawn of that slot would be
 * silently wrong; a stale handle is caught by m64_actor_resolve returning
 * NULL because the generation no longer matches.
 */
#ifndef M64_ACTOR_H
#define M64_ACTOR_H

#include <libdragon.h>
#include <t3d/t3dmath.h>

#include "m64_engine.h"
#include "m64_dict.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef M64_ACTOR_STATE_MAX
#define M64_ACTOR_STATE_MAX 64
#endif

/** Fixed draw/update order. Extend at the end; do not reorder — save data
 *  and other systems may come to depend on the numeric values. */
typedef enum {
    M64_ACTOR_CAT_PLAYER = 0,
    M64_ACTOR_CAT_ENEMY,
    M64_ACTOR_CAT_NPC,
    M64_ACTOR_CAT_PROP,
    M64_ACTOR_CAT_ITEM,
    M64_ACTOR_CAT_BOSS,
    M64_ACTOR_CAT_DOOR,
    M64_ACTOR_CAT_CHEST,
    M64_ACTOR_CATEGORY_COUNT,
} M64ActorCategory;

/** Spawn-time and runtime flags. */
enum {
    M64_ACTOR_FLAG_NONE = 0,
    M64_ACTOR_FLAG_NO_DRAW = 1 << 0, /**< updated but not drawn */
    M64_ACTOR_FLAG_PAUSED = 1 << 1,  /**< not updated (still drawn)  */
};

typedef struct M64Actor M64Actor;

typedef void (*M64ActorInitFn)(M64Actor *self, const M64Dict *spawn_args);
typedef void (*M64ActorDestroyFn)(M64Actor *self);
typedef void (*M64ActorUpdateFn)(M64Actor *self, float dt);
typedef void (*M64ActorDrawFn)(M64Actor *self);
/** Dispatched by m64_event_process when a queued event reaches its fire
 *  time. `event_id` is game-defined; `args`/`argc` are the opaque payload
 *  passed to m64_event_post. May be NULL — events queued for a profile
 *  with no event callback fire and are dropped silently. */
typedef void (*M64ActorEventFn)(M64Actor *self, uint16_t event_id,
                                const int32_t *args, uint8_t argc);

/** One entry per actor TYPE (not instance), indexed by profile_id. The
 *  game owns this table's storage and passes it to m64_actor_system_init;
 *  it must outlive the actor system. */
typedef struct {
    const char *name;    /**< for debugf / asserts, not gameplay        */
    uint8_t category;    /**< M64ActorCategory                          */
    uint16_t state_size;  /**< sizeof the type's state struct; asserted
                           *   against M64_ACTOR_STATE_MAX at init       */
    uint16_t default_flags;

    /** Called once at spawn, after pos/rot/state are zeroed and pos/rot set.
     *  May be NULL. */
    M64ActorInitFn init;
    /** Called once at despawn, before the slot returns to the free list.
     *  May be NULL. */
    M64ActorDestroyFn destroy;
    /** Called every frame the actor is alive and not M64_ACTOR_FLAG_PAUSED,
     *  in category order. May be NULL. */
    M64ActorUpdateFn update;
    /** Called every frame the actor is alive and not M64_ACTOR_FLAG_NO_DRAW,
     *  in category order, inside the 3D pass (between m64_scene_begin and
     *  m64_gui_begin). May be NULL. */
    M64ActorDrawFn draw;
    /** Called from m64_event_process when a queued event fires. May be NULL. */
    M64ActorEventFn event;
} M64ActorProfile;

#ifndef M64_ACTOR_ROOM_NONE
#define M64_ACTOR_ROOM_NONE ((uint8_t)0xFF)
#endif

/** A live actor instance. Every category shares this header; per-type data
 *  lives in `state`, cast by the type's own update/draw functions. */
struct M64Actor {
    uint16_t profile_id;
    uint8_t category;
    uint8_t flags;
    /** Owning room, used by m64_room to despawn this actor when its room
     *  unloads. M64_ACTOR_ROOM_NONE means "not owned by any room" — those
     *  actors (the player, global effects) are never auto-despawned. */
    uint8_t room_id;

    /* Pool bookkeeping. Not for game code: `next` is the intrusive link for
     * whichever list (a category list, or the free list) this slot is
     * currently in, and `generation` is bumped on every despawn so stale
     * handles resolve to NULL instead of a reused slot. */
    int16_t next;
    uint16_t generation;

    /* World transform. Embeds the engine's own M64Transform (see
     * m64_engine.h) rather than reimplementing it, so an actor draws with
     * the exact m64_transform_push/pop path any other geometry uses. */
    M64Transform xform;
    fm_vec3_t velocity;

    int32_t health; /**< -1 by convention for actors with no health concept */

    uint8_t state[M64_ACTOR_STATE_MAX];
};

/** Index + generation packed into one word. M64_ACTOR_HANDLE_NONE never
 *  resolves to a live actor. */
typedef uint32_t M64ActorHandle;
#define M64_ACTOR_HANDLE_NONE ((M64ActorHandle)0xFFFFFFFFu)

/** Bind the profile table and the (caller-owned) instance pool. Both must
 *  outlive the actor system. Asserts every profile's state_size fits
 *  M64_ACTOR_STATE_MAX — better to fail loudly at boot than corrupt a
 *  neighbouring actor's state at runtime. */
void m64_actor_system_init(const M64ActorProfile *profiles, uint16_t profile_count,
                           M64Actor *pool, uint16_t pool_capacity);

/** Allocate a slot, zero pos/rot/state, run the profile's init. Returns
 *  M64_ACTOR_HANDLE_NONE if the pool is full — checked, not asserted: a
 *  full actor pool is a runtime content fact, not a programming error.
 *  The actor's room_id is set to M64_ACTOR_ROOM_NONE; use
 *  m64_actor_spawn_in_room if this actor belongs to a streamed room.
 *  `dict` may be NULL; if non-NULL the profile's init callback can read it
 *  with m64_dict_get_* before the spawn template is freed/reused. */
M64ActorHandle m64_actor_spawn(uint16_t profile_id, fm_vec3_t pos, float yaw,
                               const M64Dict *dict);

/** As m64_actor_spawn, but tags the resulting actor with `room_id` so that
 *  m64_room_system_update despawns it when that room unloads. */
M64ActorHandle m64_actor_spawn_in_room(uint16_t profile_id, fm_vec3_t pos, float yaw,
                                       uint8_t room_id, const M64Dict *dict);

/** Run the profile's destroy, unlink from its category list, return the slot
 *  to the free list and bump its generation. Safe to call with a handle that
 *  is already stale (no-op). */
void m64_actor_despawn(M64ActorHandle h);

/** NULL if the handle is stale (despawned, or never valid). */
M64Actor *m64_actor_resolve(M64ActorHandle h);

/** The handle that resolves back to `a`. For an actor that needs to despawn
 *  itself from inside its own update/draw callback, where only the pointer
 *  is available. `a` must be a live actor obtained from this pool (spawn,
 *  resolve, or the first/next iterators) — passing a foreign pointer is a
 *  programming error and is asserted against. */
M64ActorHandle m64_actor_handle_of(const M64Actor *a);

/** Update every non-paused actor, category by category, in the fixed
 *  M64ActorCategory order. */
void m64_actor_update_all(float dt);

/** Draw every non-M64_ACTOR_FLAG_NO_DRAW actor, category by category. Call
 *  inside the 3D pass. */
void m64_actor_draw_all(void);

/** Category-scoped iteration, in list order (most-recently-spawned first).
 *  m64_actor_first(cat) returns NULL if the category is empty;
 *  m64_actor_next(cur) returns NULL after the last actor in cur's category. */
M64Actor *m64_actor_first(uint8_t category);
M64Actor *m64_actor_next(M64Actor *cur);

/** Number of live actors, all categories or just one (pass
 *  M64_ACTOR_CATEGORY_COUNT for the total). */
uint16_t m64_actor_count(uint8_t category);

/** Dispatch a queued event to `a`'s profile event callback. Used by
 *  m64_event_process; safe to call directly (no-op if the profile has no
 *  event callback). `a` must be a live actor in this pool. */
void m64_actor_dispatch_event(M64Actor *a, uint16_t event_id,
                              const int32_t *args, uint8_t argc);

#ifdef __cplusplus
}
#endif

#endif /* M64_ACTOR_H */
