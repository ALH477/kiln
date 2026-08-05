/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_tile.c — tile residency manager implementation.
 */
#include "m64_tile.h"
#include <string.h>
#include <libdragon.h>

#define SLOT_IDX(g, sx, sy) ((sy) * (g)->cfg.slots_x + (sx))
#define TILE_LOADED   0x01
#define TILE_UNLOADING 0x02

#define UNLOAD_QUEUE_CAP M64_TILE_UNLOAD_QUEUE_CAP

void m64_tile_init(M64TileManager *m,
                   const M64TileGridConfig *visual_cfg,
                   M64TileSlot *visual_slots,
                   const M64TileGridConfig *collision_cfg,
                   M64TileSlot *collision_slots,
                   M64TileLoadFn load_fn,
                   M64TileUnloadFn unload_fn,
                   M64TileSyncFn sync_fn,
                   void *user_ctx)
{
    memset(m, 0, sizeof(*m));
    m->visual.cfg = *visual_cfg;
    m->visual.slots = visual_slots;
    for (int i = 0; i < visual_cfg->slots_x * visual_cfg->slots_y; i++) {
        visual_slots[i].world_x = -1;
        visual_slots[i].world_y = -1;
    }
    if (collision_cfg && collision_slots) {
        m->collision.cfg = *collision_cfg;
        m->collision.slots = collision_slots;
        m->has_collision_grid = 1;
        for (int i = 0; i < collision_cfg->slots_x * collision_cfg->slots_y; i++) {
            collision_slots[i].world_x = -1;
            collision_slots[i].world_y = -1;
        }
    }
    m->load_fn = load_fn;
    m->unload_fn = unload_fn;
    m->sync_fn = sync_fn;
    m->user_ctx = user_ctx;
}

M64TileSlot *m64_tile_lookup(M64TileGrid *grid, int16_t tx, int16_t ty)
{
    int sx = tx % grid->cfg.slots_x;
    int sy = ty % grid->cfg.slots_y;
    if (sx < 0) sx += grid->cfg.slots_x;
    if (sy < 0) sy += grid->cfg.slots_y;
    M64TileSlot *s = &grid->slots[SLOT_IDX(grid, sx, sy)];
    if (!(s->flags & TILE_LOADED)) return NULL;
    if (s->world_x != tx || s->world_y != ty) return NULL;
    return s;
}

static void queue_unload(M64TileManager *m, M64TileGrid *grid, int slot_idx)
{
    M64TileSlot *s = &grid->slots[slot_idx];
    if (!(s->flags & TILE_LOADED) || (s->flags & TILE_UNLOADING))
        return;
    s->flags |= TILE_UNLOADING;
    if (m->unload_count < UNLOAD_QUEUE_CAP) {
        m->unload_queue[m->unload_count].grid = grid;
        m->unload_queue[m->unload_count].slot_idx = slot_idx;
        m->unload_queue[m->unload_count].saved_world_x = s->world_x;
        m->unload_queue[m->unload_count].saved_world_y = s->world_y;
        m->unload_queue[m->unload_count].saved_lod = s->lod;
        m->unload_queue[m->unload_count].saved_user_data = s->user_data;
        m->unload_count++;
    } else {
        debugf("m64_tile: unload queue full (%d entries), tile (%d,%d) "
               "will not be freed this frame\n",
               (int)UNLOAD_QUEUE_CAP, s->world_x, s->world_y);
    }
}

static void update_grid(M64TileManager *m, M64TileGrid *grid,
                        fm_vec3_t focus, M64LODSelectorFn lod_sel)
{
    M64TileGridConfig *cfg = &grid->cfg;
    int16_t ctx, cty;
    m64_tile_world_to_tile(cfg, focus, &ctx, &cty);

    int win = cfg->window_tiles;
    int min_tx = ctx - win;
    int max_tx = ctx + win;
    int min_ty = cty - win;
    int max_ty = cty + win;

    /* Clamp to world bounds. */
    if (min_tx < 0) min_tx = 0;
    if (max_tx >= cfg->tile_count_x) max_tx = cfg->tile_count_x - 1;
    if (min_ty < 0) min_ty = 0;
    if (max_ty >= cfg->tile_count_y) max_ty = cfg->tile_count_y - 1;

    /* Pass 1: mark desired tiles and load missing ones. */
    for (int ty = min_ty; ty <= max_ty; ty++) {
        for (int tx = min_tx; tx <= max_tx; tx++) {
            int sx = tx % cfg->slots_x;
            int sy = ty % cfg->slots_y;
            if (sx < 0) sx += cfg->slots_x;
            if (sy < 0) sy += cfg->slots_y;
            int idx = SLOT_IDX(grid, sx, sy);
            M64TileSlot *s = &grid->slots[idx];

            /* Desired LOD. */
            fm_vec3_t center = m64_tile_center(cfg, tx, ty);
            float dx = center.v[0] - focus.v[0];
            float dz = center.v[2] - focus.v[2];
            float dist_sq = dx * dx + dz * dz;
            uint8_t desired_lod = lod_sel ? lod_sel(tx, ty, dist_sq, m->user_ctx) : 0;

            if (desired_lod >= M64_TILE_MAX_LOD) {
                if (s->flags & TILE_LOADED && s->world_x == tx && s->world_y == ty)
                    queue_unload(m, grid, idx);
                continue;
            }

            if (s->flags & TILE_LOADED) {
                if (s->world_x == tx && s->world_y == ty) {
                    /* Already loaded — check LOD change. */
                    if (s->lod != desired_lod) {
                        /* Unload old LOD, load new. */
                        if (m->unload_fn)
                            m->unload_fn(s->world_x, s->world_y, s->lod,
                                         s->user_data, m->user_ctx);
                        s->lod = desired_lod;
                        s->user_data = m->load_fn
                            ? m->load_fn(tx, ty, desired_lod, m->user_ctx)
                            : NULL;
                    }
                    continue;
                }
                /* Slot occupied by a different tile — evict. */
                queue_unload(m, grid, idx);
            }

            /* Load the tile. */
            s->world_x = tx;
            s->world_y = ty;
            s->lod = desired_lod;
            s->generation++;
            s->user_data = m->load_fn
                ? m->load_fn(tx, ty, desired_lod, m->user_ctx)
                : NULL;
            s->flags = TILE_LOADED;
        }
    }

    /* Pass 2: queue tiles outside the window for unloading. */
    for (int i = 0; i < cfg->slots_x * cfg->slots_y; i++) {
        M64TileSlot *s = &grid->slots[i];
        if (!(s->flags & TILE_LOADED) || (s->flags & TILE_UNLOADING))
            continue;
        if (s->world_x < min_tx || s->world_x > max_tx ||
            s->world_y < min_ty || s->world_y > max_ty) {
            queue_unload(m, grid, i);
        }
    }
}

void m64_tile_update(M64TileManager *m, fm_vec3_t focus,
                      M64LODSelectorFn lod_selector)
{
    update_grid(m, &m->visual, focus, lod_selector);
    if (m->has_collision_grid)
        update_grid(m, &m->collision, focus, NULL);  /* collision always LOD 0 */
    m->visual.last_focus = focus;
    if (m->has_collision_grid)
        m->collision.last_focus = focus;
}

void m64_tile_flush_unload(M64TileManager *m)
{
    if (m->sync_fn) m->sync_fn(m->user_ctx);

    for (int i = 0; i < m->unload_count; i++) {
        M64TileGrid *grid = m->unload_queue[i].grid;
        uint8_t slot_idx = m->unload_queue[i].slot_idx;
        M64TileSlot *s = &grid->slots[slot_idx];
        if (m->unload_fn && m->unload_queue[i].saved_user_data)
            m->unload_fn(m->unload_queue[i].saved_world_x,
                         m->unload_queue[i].saved_world_y,
                         m->unload_queue[i].saved_lod,
                         m->unload_queue[i].saved_user_data,
                         m->user_ctx);
        if (s->flags & TILE_UNLOADING) {
            s->flags = 0;
            s->user_data = NULL;
            s->world_x = -1;
            s->world_y = -1;
        }
    }
    m->unload_count = 0;
}

M64TileSlot *m64_tile_first(M64TileGrid *grid)
{
    for (int i = 0; i < grid->cfg.slots_x * grid->cfg.slots_y; i++) {
        if (grid->slots[i].flags & TILE_LOADED)
            return &grid->slots[i];
    }
    return NULL;
}

M64TileSlot *m64_tile_next(M64TileGrid *grid, M64TileSlot *cur)
{
    int start = (int)(cur - grid->slots) + 1;
    for (int i = start; i < grid->cfg.slots_x * grid->cfg.slots_y; i++) {
        if (grid->slots[i].flags & TILE_LOADED)
            return &grid->slots[i];
    }
    return NULL;
}