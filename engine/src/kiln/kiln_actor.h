/* SPDX-License-Identifier: MIT
 *
 * kiln_actor.h — the actor system. Every live thing in the world (player,
 * enemies, NPCs, props, items, bosses, doors, chests) is a FigActor with a
 * shared header plus a small inline block of per-type state, registered
 * through a profile table. Modelled on Ocarina of Time's Actor/ActorProfile
 * split — see CLAUDE.md's Phase B notes for what was and wasn't carried over.
 *
 * ── Why a flat pool, not malloc per actor ─────────────────────────────────
 * The pool is caller-supplied fixed-size storage (`fig_actor_system_init`
 * takes the array, not a capacity to allocate). One bounded allocation
 * failure at boot beats a heap that can fragment mid-level. It also makes the
 * pool one contiguous, cache-friendly block on a VR4300 with 8 KB of D-cache,
 * the same reasoning streamdb-embedded's arena uses.
 *
 * ── Why per-type state is inline, not a per-type malloc ───────────────────
 * Every actor's state lives in a fixed `FIG_ACTOR_STATE_MAX`-byte block
 * inside the shared struct, sized to the largest actor type in the game
 * (override the macro before including this header if 64 bytes is too
 * small). This is OoT's "one instance struct per active actor, sized to the
 * overlay's max" idea without the overlay/segment paging machinery — see
 * CLAUDE.md for why paging is deliberately not here yet.
 *
 * ── Category lists, not one flat walk ─────────────────────────────────────
 * Actors are linked (by pool index, intrusively) into one list per
 * FigActorCategory, so `fig_actor_update_all` / `fig_actor_draw_all` visit
 * every actor in a FIXED category order — player before enemies, enemies
 * before props, etc. That ordering is also how OoT gets predictable draw
 * order and cheap category-scoped queries (fig_actor_first/next) for free.
 *
 * ── Handles, not raw pointers ──────────────────────────────────────────────
 * fig_actor_spawn returns a FigActorHandle (pool index + generation
 * counter packed into one uint32_t), not a FigActor*. A pointer an actor's
 * update function cached across a despawn/respawn of that slot would be
 * silently wrong; a stale handle is caught by fig_actor_resolve returning
 * NULL because the generation no longer matches.
 */
#ifndef FIG_ACTOR_H
#define FIG_ACTOR_H

#include <libdragon.h>
#include <t3d/t3dmath.h>

#include "kiln_engine.h"
#include "kiln_dict.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FIG_ACTOR_STATE_MAX
#define FIG_ACTOR_STATE_MAX 64
#endif

/** Fixed draw/update order. Extend at the end; do not reorder — save data
 *  and other systems may come to depend on the numeric values. */
typedef enum {
    FIG_ACTOR_CAT_PLAYER = 0,
    FIG_ACTOR_CAT_ENEMY,
    FIG_ACTOR_CAT_NPC,
    FIG_ACTOR_CAT_PROP,
    FIG_ACTOR_CAT_ITEM,
    FIG_ACTOR_CAT_BOSS,
    FIG_ACTOR_CAT_DOOR,
    FIG_ACTOR_CAT_CHEST,
    FIG_ACTOR_CATEGORY_COUNT,
} FigActorCategory;

/** Spawn-time and runtime flags. */
enum {
    FIG_ACTOR_FLAG_NONE = 0,
    FIG_ACTOR_FLAG_NO_DRAW = 1 << 0, /**< updated but not drawn */
    FIG_ACTOR_FLAG_PAUSED = 1 << 1,  /**< not updated (still drawn)  */
};

typedef struct FigActor FigActor;

typedef void (*FigActorInitFn)(FigActor *self, const FigDict *spawn_args);
typedef void (*FigActorDestroyFn)(FigActor *self);
typedef void (*FigActorUpdateFn)(FigActor *self, float dt);
typedef void (*FigActorDrawFn)(FigActor *self);
/** Dispatched by fig_event_process when a queued event reaches its fire
 *  time. `event_id` is game-defined; `args`/`argc` are the opaque payload
 *  passed to fig_event_post. May be NULL — events queued for a profile
 *  with no event callback fire and are dropped silently. */
typedef void (*FigActorEventFn)(FigActor *self, uint16_t event_id,
                                const int32_t *args, uint8_t argc);

/** One entry per actor TYPE (not instance), indexed by profile_id. The
 *  game owns this table's storage and passes it to fig_actor_system_init;
 *  it must outlive the actor system. */
typedef struct {
    const char *name;    /**< for debugf / asserts, not gameplay        */
    uint8_t category;    /**< FigActorCategory                          */
    uint16_t state_size;  /**< sizeof the type's state struct; asserted
                           *   against FIG_ACTOR_STATE_MAX at init       */
    uint16_t default_flags;

    /** Called once at spawn, after pos/rot/state are zeroed and pos/rot set.
     *  May be NULL. */
    FigActorInitFn init;
    /** Called once at despawn, before the slot returns to the free list.
     *  May be NULL. */
    FigActorDestroyFn destroy;
    /** Called every frame the actor is alive and not FIG_ACTOR_FLAG_PAUSED,
     *  in category order. May be NULL. */
    FigActorUpdateFn update;
    /** Called every frame the actor is alive and not FIG_ACTOR_FLAG_NO_DRAW,
     *  in category order, inside the 3D pass (between fig_scene_begin and
     *  fig_gui_begin). May be NULL. */
    FigActorDrawFn draw;
    /** Called from fig_event_process when a queued event fires. May be NULL. */
    FigActorEventFn event;
} FigActorProfile;

#ifndef FIG_ACTOR_ROOM_NONE
#define FIG_ACTOR_ROOM_NONE ((uint8_t)0xFF)
#endif

/** A live actor instance. Every category shares this header; per-type data
 *  lives in `state`, cast by the type's own update/draw functions. */
struct FigActor {
    uint16_t profile_id;
    uint8_t category;
    uint8_t flags;
    /** Owning room, used by fig_room to despawn this actor when its room
     *  unloads. FIG_ACTOR_ROOM_NONE means "not owned by any room" — those
     *  actors (the player, global effects) are never auto-despawned. */
    uint8_t room_id;

    /* Pool bookkeeping. Not for game code: `next` is the intrusive link for
     * whichever list (a category list, or the free list) this slot is
     * currently in, and `generation` is bumped on every despawn so stale
     * handles resolve to NULL instead of a reused slot. */
    int16_t next;
    uint16_t generation;

    /* World transform. Embeds the engine's own FigTransform (see
     * kiln_engine.h) rather than reimplementing it, so an actor draws with
     * the exact fig_transform_push/pop path any other geometry uses. */
    FigTransform xform;
    fm_vec3_t velocity;

    int32_t health; /**< -1 by convention for actors with no health concept */

    uint8_t state[FIG_ACTOR_STATE_MAX];
};

/** Index + generation packed into one word. FIG_ACTOR_HANDLE_NONE never
 *  resolves to a live actor. */
typedef uint32_t FigActorHandle;
#define FIG_ACTOR_HANDLE_NONE ((FigActorHandle)0xFFFFFFFFu)

/** Bind the profile table and the (caller-owned) instance pool. Both must
 *  outlive the actor system. Asserts every profile's state_size fits
 *  FIG_ACTOR_STATE_MAX — better to fail loudly at boot than corrupt a
 *  neighbouring actor's state at runtime. */
void fig_actor_system_init(const FigActorProfile *profiles, uint16_t profile_count,
                           FigActor *pool, uint16_t pool_capacity);

/** Allocate a slot, zero pos/rot/state, run the profile's init. Returns
 *  FIG_ACTOR_HANDLE_NONE if the pool is full — checked, not asserted: a
 *  full actor pool is a runtime content fact, not a programming error.
 *  The actor's room_id is set to FIG_ACTOR_ROOM_NONE; use
 *  fig_actor_spawn_in_room if this actor belongs to a streamed room.
 *  `dict` may be NULL; if non-NULL the profile's init callback can read it
 *  with fig_dict_get_* before the spawn template is freed/reused. */
FigActorHandle fig_actor_spawn(uint16_t profile_id, fm_vec3_t pos, float yaw,
                               const FigDict *dict);

/** As fig_actor_spawn, but tags the resulting actor with `room_id` so that
 *  fig_room_system_update despawns it when that room unloads. */
FigActorHandle fig_actor_spawn_in_room(uint16_t profile_id, fm_vec3_t pos, float yaw,
                                       uint8_t room_id, const FigDict *dict);

/** Run the profile's destroy, unlink from its category list, return the slot
 *  to the free list and bump its generation. Safe to call with a handle that
 *  is already stale (no-op). */
void fig_actor_despawn(FigActorHandle h);

/** NULL if the handle is stale (despawned, or never valid). */
FigActor *fig_actor_resolve(FigActorHandle h);

/** The handle that resolves back to `a`. For an actor that needs to despawn
 *  itself from inside its own update/draw callback, where only the pointer
 *  is available. `a` must be a live actor obtained from this pool (spawn,
 *  resolve, or the first/next iterators) — passing a foreign pointer is a
 *  programming error and is asserted against. */
FigActorHandle fig_actor_handle_of(const FigActor *a);

/** Update every non-paused actor, category by category, in the fixed
 *  FigActorCategory order. */
void fig_actor_update_all(float dt);

/** Draw every non-FIG_ACTOR_FLAG_NO_DRAW actor, category by category. Call
 *  inside the 3D pass. */
void fig_actor_draw_all(void);

/** Category-scoped iteration, in list order (most-recently-spawned first).
 *  fig_actor_first(cat) returns NULL if the category is empty;
 *  fig_actor_next(cur) returns NULL after the last actor in cur's category. */
FigActor *fig_actor_first(uint8_t category);
FigActor *fig_actor_next(FigActor *cur);

/** Number of live actors, all categories or just one (pass
 *  FIG_ACTOR_CATEGORY_COUNT for the total). */
uint16_t fig_actor_count(uint8_t category);

/** Dispatch a queued event to `a`'s profile event callback. Used by
 *  fig_event_process; safe to call directly (no-op if the profile has no
 *  event callback). `a` must be a live actor in this pool. */
void fig_actor_dispatch_event(FigActor *a, uint16_t event_id,
                              const int32_t *args, uint8_t argc);

#ifdef __cplusplus
}
#endif

#endif /* FIG_ACTOR_H */
