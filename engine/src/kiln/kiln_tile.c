/* SPDX-License-Identifier: MIT
 *
 * kiln_tile.c — tile residency manager implementation.
 */
#include "kiln_tile.h"
#include <string.h>
#include <libdragon.h>

#define SLOT_IDX(g, sx, sy) ((sy) * (g)->cfg.slots_x + (sx))
#define TILE_LOADED    0x01
#define TILE_UNLOADING 0x02
#define TILE_PENDING   0x04

#define UNLOAD_QUEUE_CAP KILN_TILE_UNLOAD_QUEUE_CAP

void kiln_tile_init(KilnTileManager *m,
                   const KilnTileGridConfig *visual_cfg,
                   KilnTileSlot *visual_slots,
                   const KilnTileGridConfig *collision_cfg,
                   KilnTileSlot *collision_slots,
                   KilnTileLoadFn load_fn,
                   KilnTileUnloadFn unload_fn,
                   KilnTileSyncFn sync_fn,
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
    m->load_budget = 2;
}

KilnTileSlot *kiln_tile_lookup(KilnTileGrid *grid, int16_t tx, int16_t ty)
{
    int sx = tx % grid->cfg.slots_x;
    int sy = ty % grid->cfg.slots_y;
    if (sx < 0) sx += grid->cfg.slots_x;
    if (sy < 0) sy += grid->cfg.slots_y;
    KilnTileSlot *s = &grid->slots[SLOT_IDX(grid, sx, sy)];
    if (!(s->flags & TILE_LOADED)) return NULL;
    if (s->world_x != tx || s->world_y != ty) return NULL;
    return s;
}

static void queue_unload(KilnTileManager *m, KilnTileGrid *grid, int slot_idx)
{
    KilnTileSlot *s = &grid->slots[slot_idx];
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
        debugf("kiln_tile: unload queue full (%d entries), tile (%d,%d) "
               "will not be freed this frame\n",
               (int)UNLOAD_QUEUE_CAP, s->world_x, s->world_y);
    }
}

static void update_grid(KilnTileManager *m, KilnTileGrid *grid,
                        fm_vec3_t focus, KilnLODSelectorFn lod_sel,
                        uint8_t *load_count)
{
    KilnTileGridConfig *cfg = &grid->cfg;
    int16_t ctx, cty;
    kiln_tile_world_to_tile(cfg, focus, &ctx, &cty);

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

    /* Pass 0: service tiles whose load was deferred by the budget on a
     * previous frame. These are still inside the window (they were
     * desired then and haven't scrolled out yet), so load them first. */
    for (int i = 0; i < cfg->slots_x * cfg->slots_y; i++) {
        KilnTileSlot *s = &grid->slots[i];
        if (!(s->flags & TILE_PENDING))
            continue;
        if (*load_count >= m->load_budget)
            break;
        s->user_data = m->load_fn
            ? m->load_fn(s->world_x, s->world_y, s->lod, m->user_ctx)
            : NULL;
        s->flags = TILE_LOADED;
        (*load_count)++;
    }

    /* Pass 1: mark desired tiles and load missing ones. */
    for (int ty = min_ty; ty <= max_ty; ty++) {
        for (int tx = min_tx; tx <= max_tx; tx++) {
            int sx = tx % cfg->slots_x;
            int sy = ty % cfg->slots_y;
            if (sx < 0) sx += cfg->slots_x;
            if (sy < 0) sy += cfg->slots_y;
            int idx = SLOT_IDX(grid, sx, sy);
            KilnTileSlot *s = &grid->slots[idx];

            /* Desired LOD. */
            fm_vec3_t center = kiln_tile_center(cfg, tx, ty);
            float dx = center.v[0] - focus.v[0];
            float dz = center.v[2] - focus.v[2];
            float dist_sq = dx * dx + dz * dz;
            uint8_t desired_lod = lod_sel ? lod_sel(tx, ty, dist_sq, m->user_ctx) : 0;

            if (desired_lod >= KILN_TILE_MAX_LOD) {
                if (s->flags & TILE_LOADED && s->world_x == tx && s->world_y == ty)
                    queue_unload(m, grid, idx);
                continue;
            }

            if (s->flags & TILE_LOADED) {
                if (s->world_x == tx && s->world_y == ty) {
                    /* Already loaded — check LOD change. */
                    if (s->lod != desired_lod) {
                        if (s->flags & TILE_PENDING) {
                            /* No data loaded yet — just update desired LOD. */
                            s->lod = desired_lod;
                        } else if (*load_count < m->load_budget) {
                            /* Budget available — unload old LOD, load new. */
                            if (m->unload_fn)
                                m->unload_fn(s->world_x, s->world_y, s->lod,
                                             s->user_data, m->user_ctx);
                            s->lod = desired_lod;
                            s->user_data = m->load_fn
                                ? m->load_fn(tx, ty, desired_lod, m->user_ctx)
                                : NULL;
                            (*load_count)++;
                        }
                        /* Budget exhausted: keep current LOD, retry next frame. */
                    }
                    continue;
                }
                /* Slot occupied by a different tile — evict. */
                queue_unload(m, grid, idx);
            }

            /* Load the tile (or defer if budget exhausted). */
            s->world_x = tx;
            s->world_y = ty;
            s->lod = desired_lod;
            s->generation++;
            if (*load_count < m->load_budget) {
                s->user_data = m->load_fn
                    ? m->load_fn(tx, ty, desired_lod, m->user_ctx)
                    : NULL;
                s->flags = TILE_LOADED;
                (*load_count)++;
            } else {
                s->user_data = NULL;
                s->flags = TILE_LOADED | TILE_PENDING;
            }
        }
    }

    /* Pass 2: queue tiles outside the window for unloading. */
    for (int i = 0; i < cfg->slots_x * cfg->slots_y; i++) {
        KilnTileSlot *s = &grid->slots[i];
        if (!(s->flags & TILE_LOADED) || (s->flags & TILE_UNLOADING))
            continue;
        if (s->world_x < min_tx || s->world_x > max_tx ||
            s->world_y < min_ty || s->world_y > max_ty) {
            queue_unload(m, grid, i);
        }
    }
}

void kiln_tile_update(KilnTileManager *m, fm_vec3_t focus,
                      KilnLODSelectorFn lod_selector)
{
    uint8_t load_count = 0;
    update_grid(m, &m->visual, focus, lod_selector, &load_count);
    if (m->has_collision_grid)
        update_grid(m, &m->collision, focus, NULL, &load_count);  /* collision always LOD 0 */
    m->visual.last_focus = focus;
    if (m->has_collision_grid)
        m->collision.last_focus = focus;
}

void kiln_tile_flush_unload(KilnTileManager *m)
{
    if (m->sync_fn) m->sync_fn(m->user_ctx);

    for (int i = 0; i < m->unload_count; i++) {
        KilnTileGrid *grid = m->unload_queue[i].grid;
        uint8_t slot_idx = m->unload_queue[i].slot_idx;
        KilnTileSlot *s = &grid->slots[slot_idx];
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

KilnTileSlot *kiln_tile_first(KilnTileGrid *grid)
{
    for (int i = 0; i < grid->cfg.slots_x * grid->cfg.slots_y; i++) {
        if ((grid->slots[i].flags & TILE_LOADED) &&
            !(grid->slots[i].flags & TILE_PENDING))
            return &grid->slots[i];
    }
    return NULL;
}

KilnTileSlot *kiln_tile_next(KilnTileGrid *grid, KilnTileSlot *cur)
{
    int start = (int)(cur - grid->slots) + 1;
    for (int i = start; i < grid->cfg.slots_x * grid->cfg.slots_y; i++) {
        if ((grid->slots[i].flags & TILE_LOADED) &&
            !(grid->slots[i].flags & TILE_PENDING))
            return &grid->slots[i];
    }
    return NULL;
}