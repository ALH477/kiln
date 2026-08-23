/* SPDX-License-Identifier: MIT
 *
 * kiln_streamio.h — binds kiln_stream's admission policy to real
 * kiln_asset + kiln_cache loads, and adapts it to kiln_room / kiln_tile's
 * existing callback contracts.
 *
 * Compiles natively (kiln_asset, kiln_room, kiln_tile and kiln_cache are all
 * host-buildable now that plat/host/include ships a t3dmodel.h shim), so
 * both this and kiln_stream sit in HOST_MODULES and get the -Werror second
 * opinion from nix/checks/kiln-parity.nix. Only kiln_stream's admission
 * POLICY is asserted on at runtime by nix/checks/kiln-logic.nix, though —
 * this file's kiln_asset_model/kiln_asset_sprite/t3d_model_free/sprite_free
 * calls need real StreamDB content and a linked host_t3dmodel.c to exercise
 * end to end, which is a heavier host check left as a follow-on (see the
 * plan's "optional follow-on" note) rather than required for this to land.
 *
 * ── Zero new callback contract ──────────────────────────────────────────
 * kiln_streamio_room_on_load/_on_unload and kiln_streamio_tile_on_load/
 * _on_unload are literal KilnRoomLoadFn/UnloadFn and KilnTileLoadFn/UnloadFn
 * implementations — a game that wants paced streaming points its existing
 * kiln_room_system_init / kiln_tile_init callback slots at these instead of
 * writing its own; a game that doesn't need pacing keeps its own loaders
 * exactly as before. No edits to kiln_room.h or kiln_tile.h were needed or
 * made.
 *
 * ── The kiln_tile load_budget = 255 requirement ─────────────────────────
 * kiln_tile's own load_fn is called SYNCHRONOUSLY and is expected to return
 * the loaded user_data pointer immediately — but the whole point here is
 * that the real, expensive kiln_asset+kiln_cache call must NOT happen
 * synchronously inside that callback, or kiln_stream's budget/priority
 * ordering would have nothing left to pace. So kiln_streamio_tile_on_load
 * does no I/O: it only calls kiln_stream_request (an O(1) array insert) and
 * returns NULL. For that to run for every newly-desired tile in the same
 * frame — rather than kiln_tile deferring some of them to TILE_PENDING on
 * its OWN flat per-frame counter — the manager must be initialised with
 * `tiles.load_budget = 255`, kiln_tile.h's own documented "synchronous,
 * load-all-immediately" escape hatch. With that set, kiln_tile's throttling
 * never engages and kiln_stream becomes the sole budget authority; the real
 * load happens later, in kiln_streamio_pump, only for admitted requests.
 * kiln_streamio_room_on_load needs no such setting — kiln_room has no
 * budget of its own to disable.
 *
 * ── How a completed load reaches room->user_mesh / tile->user_data ──────
 * Neither field is written from inside on_load (which returns before the
 * real load has even been requested for admission). kiln_streamio_pump
 * writes them once the admitted request's kiln_cache_acquire call actually
 * completes, later the same frame, before the draw pass runs. This is safe
 * because both fields are already documented as "the engine never
 * dereferences this; the user's own draw callback does, after checking for
 * NULL" — exactly the state a tile/room whose load is still outstanding is
 * already expected to tolerate.
 */
#ifndef KILN_STREAMIO_H
#define KILN_STREAMIO_H

#include <stdint.h>

#include "kiln_stream.h"
#include "kiln_asset.h"
#include "kiln_cache.h"
#include "kiln_room.h"
#include "kiln_tile.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Binds one KilnStream to one asset DB + cache. Own nothing: `db` and
 *  `cache` are caller-opened/initialised and must outlive this. */
typedef struct {
    KilnAsset *db;
    KilnCache *cache;
    KilnStream stream;
    uint32_t   fail_total;  /**< an ADMITTED request's kiln_asset_model/
                                 sprite call returned NULL — a content bug
                                 (bad key, missing DB entry), not a budget
                                 one. Distinct RED gauge from
                                 kiln_stream_dropped_total, which is a
                                 capacity/tuning problem instead. */
} KilnStreamIO;

void kiln_streamio_init(KilnStreamIO *io, KilnAsset *db, KilnCache *cache,
                        KilnStreamBudget budget);

/** Call once per frame: after kiln_stream_frame_begin (implicitly run
 *  inside here) and after this frame's room/tile on_load calls have
 *  submitted their requests, before the draw pass. Issues the real
 *  kiln_asset + kiln_cache call for every request kiln_stream admitted this
 *  frame, writes the result into the owning room's user_mesh or tile's
 *  user_data, and completes the request either way (a genuine asset
 *  failure must not retry forever — see kiln_stream_complete). */
void kiln_streamio_pump(KilnStreamIO *io);

uint32_t kiln_streamio_fail_total(const KilnStreamIO *io);

/** Which kiln_asset accessor a given key resolves through. */
typedef enum {
    KILN_STREAMIO_MODEL = 0,   /**< kiln_asset_model / t3d_model_free */
    KILN_STREAMIO_SPRITE = 1,  /**< kiln_asset_sprite / sprite_free   */
} KilnStreamIOAssetType;

/** Per-room/per-tile bookkeeping: which cache handle (if any) is resident,
 *  which kiln_stream request (if any) is still outstanding, and enough to
 *  find the owner again when kiln_streamio_pump resolves it later. Exactly
 *  one of `handle`/`req` is ever live at a time (never both INVALID after
 *  on_load, and both INVALID once unloaded). Caller-allocated: one array
 *  entry per KilnRoom (indexed by room->id) or per tile grid slot (same
 *  slots_x*slots_y sizing and modulo-wrap indexing as KilnTileGrid itself,
 *  per kiln_tile.h's "Slot reuse via modulo wrapping" section). */
typedef struct {
    KilnCacheHandle  handle;
    KilnStreamHandle req;
    KilnStreamIOAssetType asset_type;
    enum { KILN_STREAMIO_OWNER_ROOM, KILN_STREAMIO_OWNER_TILE } owner_kind;
    union {
        struct { KilnRoom *room; } room;
        struct { KilnTileGrid *grid; int16_t tx, ty; } tile;
    } owner;
} KilnStreamIOSlot;

/* ── Room adapter ───────────────────────────────────────────────────── */

/** Builds the StreamDB key for a room's mesh asset. Game-specific — e.g.
 *  snprintf(out, out_len, "rooms/%02u.t3dm", room->id). */
typedef void (*KilnStreamIORoomKeyFn)(const KilnRoom *room, char *out, size_t out_len);

typedef struct {
    KilnStreamIO *io;
    KilnStreamIOSlot *slots;         /**< caller-allocated, room_count entries */
    KilnStreamIOAssetType asset_type;
    KilnStreamIORoomKeyFn key_fn;
    fm_vec3_t camera_pos;            /**< set by the caller once per frame,
                                          before kiln_room_system_update */
} KilnStreamIORoomBinding;

/** Zeroes `slots` (room_count entries) and binds the callbacks below to
 *  route through `io`. Call once at boot, before kiln_room_system_init. */
void kiln_streamio_room_bind(KilnStreamIORoomBinding *b, KilnStreamIO *io,
                             KilnStreamIOSlot *slots, uint16_t room_count,
                             KilnStreamIOAssetType type,
                             KilnStreamIORoomKeyFn key_fn);

/** Pass as KilnRoomLoadFn/UnloadFn to kiln_room_system_init with
 *  user_ctx = &binding. */
void kiln_streamio_room_on_load  (KilnRoom *room, void *user);
void kiln_streamio_room_on_unload(KilnRoom *room, void *user);

/* ── Tile adapter ───────────────────────────────────────────────────── */

/** Builds the StreamDB key for a tile at (tx, ty, lod). Game-specific —
 *  e.g. snprintf(out, out_len, "tiles/%d_%d/lod%u.t3dm", tx, ty, lod). */
typedef void (*KilnStreamIOTileKeyFn)(int16_t tx, int16_t ty, uint8_t lod,
                                      char *out, size_t out_len);

typedef struct {
    KilnStreamIO *io;
    KilnTileGrid *grid;               /**< the grid this binding services */
    KilnStreamIOSlot *slots;          /**< caller-allocated,
                                            slots_x * slots_y entries      */
    KilnStreamIOAssetType asset_type;
    KilnStreamIOTileKeyFn key_fn;
    fm_vec3_t camera_pos;             /**< set by the caller once per frame,
                                            before kiln_tile_update */
} KilnStreamIOTileBinding;

/** Zeroes `slots` (grid->cfg.slots_x * grid->cfg.slots_y entries). Call
 *  once at boot, before kiln_tile_init. `grid` must be the SAME grid the
 *  manager is later initialised with — the slot indexing has to agree. */
void kiln_streamio_tile_bind(KilnStreamIOTileBinding *b, KilnStreamIO *io,
                             KilnTileGrid *grid, KilnStreamIOSlot *slots,
                             KilnStreamIOAssetType type,
                             KilnStreamIOTileKeyFn key_fn);

/** Pass as KilnTileLoadFn/UnloadFn to kiln_tile_init with
 *  user_ctx = &binding. REQUIRES the manager's load_budget to be set to
 *  255 (see the file comment) — kiln_tile_init defaults to 2, which must
 *  be overridden after init, before the first kiln_tile_update call. */
void *kiln_streamio_tile_on_load  (int16_t tx, int16_t ty, uint8_t lod, void *user);
void  kiln_streamio_tile_on_unload(int16_t tx, int16_t ty, uint8_t lod,
                                   void *user_data, void *user);

#ifdef __cplusplus
}
#endif

#endif /* KILN_STREAMIO_H */
