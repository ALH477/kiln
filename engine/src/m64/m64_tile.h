/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_tile.h — tile residency manager for open-world streaming.
 *
 * ── Why tiles instead of rooms ─────────────────────────────────────────
 * The existing m64_room system partitions the world into named rooms with
 * AABB-overlap + neighbour tables. That is the right model for OoT-style
 * dungeon rooms: discrete, hand-authored, few in number. An open world
 * needs the opposite: a uniform grid of small tiles, streamed in and out
 * based on camera position, with multiple LOD levels per tile. Tiles and
 * rooms coexist — rooms handle actor spawning, collision, and audio
 * routing; tiles handle geometry LOD and far/near rendering. A room owns
 * the tiles that overlap its AABB; the tile system manages which LOD of
 * each tile is resident.
 *
 * ── Slot reuse via modulo wrapping ─────────────────────────────────────
 * The world may be larger than the loaded window, but the loaded window
 * is small (e.g. 5×5 tiles). We allocate a fixed 2D array of slots and
 * map world tile coordinates into slots via modulo: slot = (tx % W, ty % H).
 * This is the same trick Junkrunner64 uses — it means tile (0,0) and
 * tile (W,0) share a slot, but only one is loaded at a time because the
 * window is smaller than W. The slot stores the world coordinates of its
 * current occupant so a stale slot is detected by coordinate mismatch.
 *
 * ── Unload queue ──────────────────────────────────────────────────────
 * Tiles that scroll out of the window are not freed immediately — their
 * display lists may still be executing on the RDP. Instead they are
 * placed on a bounded unload queue. The queue is flushed at the start of
 * the next frame (or after an explicit GPU sync), after which the RSP is
 * guaranteed to be done with those display lists. This mirrors the
 * rspq_wait + deferred-free pattern in Junkrunner64's overworld_check_unload_queue.
 *
 * ── LOD levels ────────────────────────────────────────────────────────
 * Each tile can have multiple LOD levels (lod0 = highest detail, lod1 =
 * lower, etc.). The residency manager tracks which LOD is currently loaded
 * per slot. The LOD selector (m64_lod.h) decides which LOD to request; the
 * residency manager executes the load/unload. A tile can be promoted from
 * lod1 to lod0 by loading lod0 on top and freeing lod1, or demoted by the
 * reverse. A tile that scrolls out entirely is freed regardless of LOD.
 *
 * ── Separate visual and collision tiles ────────────────────────────────
 * Visual tiles (geometry + materials) and collision tiles (brushes, nav
 * meshes) have different residency requirements: collision tiles need a
 * larger radius (the player can collide with things they can't see yet),
 * while visual tiles can be tighter around the camera. The manager
 * supports two independent grids with different window sizes, or a single
 * grid if the game doesn't need the split.
 *
 * ── Backend-agnostic loading ───────────────────────────────────────────
 * The manager never opens files or StreamDB directly. Load and unload are
 * caller-supplied callbacks that receive the tile coordinates and LOD
 * level. This means the same manager works on top of DFS, StreamDB, or a
 * future host-side asset fabric. The caller is responsible for populating
 * the tile's user_data pointer (e.g. with a T3DModel* or brush array).
 *
 * Inspired by lambertjamesd/n64brew2025's overworld system but designed
 * from first principles: no FILE* or fseek, no hardcoded binary format,
 * generation-counted slots, explicit LOD support, and separate
 * visual/collision grids.
 */
#ifndef M64_TILE_H
#define M64_TILE_H

#include <t3d/t3dmath.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum LOD levels per tile. 3 is enough for near/mid/far. */
#define M64_TILE_MAX_LOD 3

/** Unload queue capacity. 5×5 window (25 tiles) × 3 LOD + margin for LOD promotions. */
#define M64_TILE_UNLOAD_QUEUE_CAP 96

/** A loaded tile. The user_data pointer is set by the load callback and
 *  read by the draw callback — the manager never dereferences it. */
typedef struct {
    int16_t  world_x;       /**< world tile X coordinate, or -1 if empty  */
    int16_t  world_y;       /**< world tile Y coordinate, or -1 if empty  */
    uint8_t  lod;           /**< current LOD level (0 = highest detail)    */
    uint8_t  generation;   /**< incremented when the slot is reused       */
    uint8_t  flags;         /**< bit 0 = loaded, bit 1 = unload-pending,
                                  bit 2 = load-pending (data not yet avail) */
    void    *user_data;     /**< caller-owned (e.g. T3DModel*, M64Brush*) */
} M64TileSlot;

/** Per-grid configuration. */
typedef struct {
    uint16_t  tile_count_x;     /**< world size in tiles (X axis)         */
    uint16_t  tile_count_y;     /**< world size in tiles (Y axis)         */
    float     tile_size;        /**< world units per tile                  */
    fm_vec3_t origin;           /**< world position of tile (0,0) corner  */
    uint8_t   window_tiles;     /**< loaded window half-size in tiles      */
    uint8_t   slots_x;          /**< slot array width (must be >= window)  */
    uint8_t   slots_y;          /**< slot array height (must be >= window) */
} M64TileGridConfig;

/** One grid (visual or collision). */
typedef struct {
    M64TileGridConfig cfg;
    M64TileSlot      *slots;     /**< slots_x × slots_y, caller-allocated  */
    fm_vec3_t         last_focus; /**< last camera position passed to update */
} M64TileGrid;

/** Load callback: called when a tile needs to be loaded at a given LOD.
 *  Returns the user_data pointer (e.g. a loaded model), or NULL on failure.
 *  The manager stores this in the slot's user_data. */
typedef void *(*M64TileLoadFn)(int16_t tx, int16_t ty, uint8_t lod,
                               void *user_ctx);

/** Unload callback: called when a tile is evicted. The callback should free
 *  the resource pointed to by user_data. */
typedef void (*M64TileUnloadFn)(int16_t tx, int16_t ty, uint8_t lod,
                                void *user_data, void *user_ctx);

/** Unload-queue flush callback: called once before the unload queue is
 *  processed, to let the caller issue a GPU sync (e.g. rspq_wait).
 *  May be NULL if no GPU sync is needed. */
typedef void (*M64TileSyncFn)(void *user_ctx);

/** The residency manager. Owns up to 2 grids (visual + collision). */
typedef struct {
    M64TileGrid visual;
    M64TileGrid collision;
    int has_collision_grid;

    /* Unload queue (shared between both grids). */
    struct {
        M64TileGrid *grid;
        uint8_t      slot_idx;          /**< index into grid->slots          */
        int16_t      saved_world_x;     /**< slot state at queue time        */
        int16_t      saved_world_y;
        uint8_t      saved_lod;
        void        *saved_user_data;
    } unload_queue[M64_TILE_UNLOAD_QUEUE_CAP];
    uint8_t unload_count;

    /** Max new tile loads per frame; excess tiles are marked TILE_PENDING
     *  and loaded on subsequent frames. Default 2. Set to 255 for
     *  synchronous (load-all-immediately) behaviour. */
    uint8_t load_budget;

    M64TileLoadFn   load_fn;
    M64TileUnloadFn unload_fn;
    M64TileSyncFn   sync_fn;
    void           *user_ctx;
} M64TileManager;

/** Initialise the manager with one or two grids. The slot arrays must be
 *  pre-allocated by the caller (slots_x × slots_y sizeof(M64TileSlot) each)
 *  and zeroed. Pass collision_cfg = NULL for a single visual grid. */
void m64_tile_init(M64TileManager *m,
                   const M64TileGridConfig *visual_cfg,
                   M64TileSlot *visual_slots,
                   const M64TileGridConfig *collision_cfg,
                   M64TileSlot *collision_slots,
                   M64TileLoadFn load_fn,
                   M64TileUnloadFn unload_fn,
                   M64TileSyncFn sync_fn,
                   void *user_ctx);

/** Per-frame update: compute the desired tile set from the camera position,
 *  load tiles that entered the window, queue tiles that left for unloading,
 *  and promote/demote LOD levels. Call once per frame before rendering.
 *
 *  `lod_selector` is called per tile to determine the desired LOD level;
 *  pass NULL to always use LOD 0. */
typedef uint8_t (*M64LODSelectorFn)(int16_t tx, int16_t ty,
                                     float dist_sq, void *user_ctx);

void m64_tile_update(M64TileManager *m, fm_vec3_t focus,
                      M64LODSelectorFn lod_selector);

/** Flush the unload queue. Called at the start of the next frame (after
 *  the GPU sync callback has been invoked). Frees the resources of tiles
 *  that scrolled out of the window. */
void m64_tile_flush_unload(M64TileManager *m);

/** Look up a loaded tile by world coordinates. Returns NULL if the tile
 *  is not currently resident. */
M64TileSlot *m64_tile_lookup(M64TileGrid *grid, int16_t tx, int16_t ty);

/** Iterate loaded tiles in slot order. Tiles with TILE_PENDING (load
 *  deferred by the budget) are skipped — the draw callback never sees
 *  a tile whose user_data has not been populated yet. Use
 *  m64_tile_lookup to poll a specific tile's status regardless. */
M64TileSlot *m64_tile_first(M64TileGrid *grid);
M64TileSlot *m64_tile_next(M64TileGrid *grid, M64TileSlot *cur);

/** World position helpers. */
static inline fm_vec3_t m64_tile_center(const M64TileGridConfig *cfg,
                                          int16_t tx, int16_t ty) {
    fm_vec3_t p = {{
        cfg->origin.v[0] + (tx + 0.5f) * cfg->tile_size,
        cfg->origin.v[1],
        cfg->origin.v[2] + (ty + 0.5f) * cfg->tile_size,
    }};
    return p;
}

/** Convert a world position to tile coordinates. */
static inline void m64_tile_world_to_tile(const M64TileGridConfig *cfg,
                                           fm_vec3_t pos,
                                           int16_t *tx, int16_t *ty) {
    float fx = (pos.v[0] - cfg->origin.v[0]) / cfg->tile_size;
    float fy = (pos.v[2] - cfg->origin.v[2]) / cfg->tile_size;
    *tx = (int16_t)fx;
    *ty = (int16_t)fy;
}

#ifdef __cplusplus
}
#endif

#endif /* M64_TILE_H */