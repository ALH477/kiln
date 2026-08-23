/* SPDX-License-Identifier: MIT
 *
 * kiln_streamio.c — see kiln_streamio.h for the model.
 */

#include "kiln_streamio.h"

#include <string.h>
#include <libdragon.h>

/* ── kiln_cache load/release wrappers, one pair per asset type ────────── */

static void *cache_load_model(const char *key, void *ctx)
{
    KilnAsset *db = (KilnAsset *)ctx;
    return kiln_asset_model(db, key, strlen(key));
}

static void cache_release_model(void *resource, void *ctx)
{
    (void)ctx;
    t3d_model_free((T3DModel *)resource);
}

static void *cache_load_sprite(const char *key, void *ctx)
{
    KilnAsset *db = (KilnAsset *)ctx;
    return kiln_asset_sprite(db, key, strlen(key));
}

static void cache_release_sprite(void *resource, void *ctx)
{
    (void)ctx;
    sprite_free((sprite_t *)resource);
}

static KilnCacheLoadFn load_fn_for(KilnStreamIOAssetType t)
{
    return (t == KILN_STREAMIO_SPRITE) ? cache_load_sprite : cache_load_model;
}

static KilnCacheReleaseFn release_fn_for(KilnStreamIOAssetType t)
{
    return (t == KILN_STREAMIO_SPRITE) ? cache_release_sprite : cache_release_model;
}

/* ── KilnStreamIO ───────────────────────────────────────────────────────── */

void kiln_streamio_init(KilnStreamIO *io, KilnAsset *db, KilnCache *cache,
                        KilnStreamBudget budget)
{
    io->db = db;
    io->cache = cache;
    io->fail_total = 0;
    kiln_stream_init(&io->stream, budget);
}

void kiln_streamio_pump(KilnStreamIO *io)
{
    kiln_stream_frame_begin(&io->stream);

    for (KilnStreamSlot *adm = kiln_stream_first_admitted(&io->stream); adm;
         adm = kiln_stream_next_admitted(&io->stream, adm)) {
        KilnStreamIOSlot *iot = (KilnStreamIOSlot *)adm->tag;

        KilnCacheHandle h = kiln_cache_acquire(io->cache, adm->key,
                                             load_fn_for(iot->asset_type),
                                             release_fn_for(iot->asset_type),
                                             io->db);
        void *resource = NULL;
        if (h == KILN_CACHE_HANDLE_INVALID) {
            io->fail_total++;
            debugf("kiln_streamio: load failed for '%s'\n", adm->key);
        } else {
            iot->handle = h;
            resource = kiln_cache_resolve(io->cache, h);
        }

        switch (iot->owner_kind) {
        case KILN_STREAMIO_OWNER_ROOM:
            iot->owner.room.room->user_mesh = resource;
            break;
        case KILN_STREAMIO_OWNER_TILE: {
            KilnTileSlot *ts = kiln_tile_lookup(iot->owner.tile.grid,
                                               iot->owner.tile.tx,
                                               iot->owner.tile.ty);
            /* NULL means the tile scrolled out (or was reused) before its
             * load resolved — the cache handle above (if any) is still
             * correctly held by iot->handle and will be released normally
             * when on_unload eventually runs for it. */
            if (ts) ts->user_data = resource;
            break;
        }
        }

        KilnStreamHandle req = iot->req;
        iot->req = KILN_STREAM_HANDLE_INVALID;
        kiln_stream_complete(&io->stream, req);
    }
}

uint32_t kiln_streamio_fail_total(const KilnStreamIO *io)
{
    return io->fail_total;
}

/* ── Room adapter ───────────────────────────────────────────────────────── */

void kiln_streamio_room_bind(KilnStreamIORoomBinding *b, KilnStreamIO *io,
                             KilnStreamIOSlot *slots, uint16_t room_count,
                             KilnStreamIOAssetType type,
                             KilnStreamIORoomKeyFn key_fn)
{
    b->io = io;
    b->slots = slots;
    b->asset_type = type;
    b->key_fn = key_fn;
    memset(&b->camera_pos, 0, sizeof(b->camera_pos));

    memset(slots, 0, sizeof(KilnStreamIOSlot) * room_count);
    for (uint16_t i = 0; i < room_count; i++) {
        slots[i].handle = KILN_CACHE_HANDLE_INVALID;
        slots[i].req = KILN_STREAM_HANDLE_INVALID;
    }
}

void kiln_streamio_room_on_load(KilnRoom *room, void *user)
{
    KilnStreamIORoomBinding *b = (KilnStreamIORoomBinding *)user;
    KilnStreamIOSlot *slot = &b->slots[room->id];

    char key[KILN_STREAM_MAX_KEY_LEN];
    b->key_fn(room, key, sizeof(key));

    slot->handle = KILN_CACHE_HANDLE_INVALID;
    slot->asset_type = b->asset_type;
    slot->owner_kind = KILN_STREAMIO_OWNER_ROOM;
    slot->owner.room.room = room;

    float cx = (room->aabb_min.v[0] + room->aabb_max.v[0]) * 0.5f - b->camera_pos.v[0];
    float cy = (room->aabb_min.v[1] + room->aabb_max.v[1]) * 0.5f - b->camera_pos.v[1];
    float cz = (room->aabb_min.v[2] + room->aabb_max.v[2]) * 0.5f - b->camera_pos.v[2];
    float dist_sq = cx * cx + cy * cy + cz * cz;

    size_t need = kiln_asset_size(b->io->db, key, strlen(key));
    slot->req = kiln_stream_request(&b->io->stream, key, KILN_STREAM_NORMAL,
                                    dist_sq, (uint32_t)need, slot);

    room->user_mesh = NULL; /* documented init state; kiln_streamio_pump fills it in */
}

void kiln_streamio_room_on_unload(KilnRoom *room, void *user)
{
    KilnStreamIORoomBinding *b = (KilnStreamIORoomBinding *)user;
    KilnStreamIOSlot *slot = &b->slots[room->id];

    if (slot->handle != KILN_CACHE_HANDLE_INVALID) {
        kiln_cache_release(b->io->cache, slot->handle,
                          release_fn_for(slot->asset_type), b->io->db);
        slot->handle = KILN_CACHE_HANDLE_INVALID;
    } else if (slot->req != KILN_STREAM_HANDLE_INVALID) {
        kiln_stream_cancel(&b->io->stream, slot->req);
        slot->req = KILN_STREAM_HANDLE_INVALID;
    }
    room->user_mesh = NULL;
}

/* ── Tile adapter ───────────────────────────────────────────────────────── */

/* Same modulo-wrap slot indexing kiln_tile.c uses internally, and the exact
 * scheme kiln_tile.h's "Slot reuse via modulo wrapping" section documents
 * as this module's public contract — so this stays in lockstep with the
 * manager's own slot for (tx, ty) without needing kiln_tile.h to expose the
 * index directly. */
static int tile_slot_index(const KilnTileGrid *grid, int16_t tx, int16_t ty)
{
    int sx = tx % grid->cfg.slots_x;
    int sy = ty % grid->cfg.slots_y;
    if (sx < 0) sx += grid->cfg.slots_x;
    if (sy < 0) sy += grid->cfg.slots_y;
    return sy * grid->cfg.slots_x + sx;
}

void kiln_streamio_tile_bind(KilnStreamIOTileBinding *b, KilnStreamIO *io,
                             KilnTileGrid *grid, KilnStreamIOSlot *slots,
                             KilnStreamIOAssetType type,
                             KilnStreamIOTileKeyFn key_fn)
{
    b->io = io;
    b->grid = grid;
    b->slots = slots;
    b->asset_type = type;
    b->key_fn = key_fn;
    memset(&b->camera_pos, 0, sizeof(b->camera_pos));

    int n = (int)grid->cfg.slots_x * (int)grid->cfg.slots_y;
    memset(slots, 0, sizeof(KilnStreamIOSlot) * (size_t)n);
    for (int i = 0; i < n; i++) {
        slots[i].handle = KILN_CACHE_HANDLE_INVALID;
        slots[i].req = KILN_STREAM_HANDLE_INVALID;
    }
}

void *kiln_streamio_tile_on_load(int16_t tx, int16_t ty, uint8_t lod, void *user)
{
    KilnStreamIOTileBinding *b = (KilnStreamIOTileBinding *)user;
    KilnStreamIOSlot *slot = &b->slots[tile_slot_index(b->grid, tx, ty)];

    char key[KILN_STREAM_MAX_KEY_LEN];
    b->key_fn(tx, ty, lod, key, sizeof(key));

    slot->handle = KILN_CACHE_HANDLE_INVALID;
    slot->asset_type = b->asset_type;
    slot->owner_kind = KILN_STREAMIO_OWNER_TILE;
    slot->owner.tile.grid = b->grid;
    slot->owner.tile.tx = tx;
    slot->owner.tile.ty = ty;

    fm_vec3_t center = kiln_tile_center(&b->grid->cfg, tx, ty);
    float dx = center.v[0] - b->camera_pos.v[0];
    float dz = center.v[2] - b->camera_pos.v[2];
    float dist_sq = dx * dx + dz * dz;

    size_t need = kiln_asset_size(b->io->db, key, strlen(key));
    /* lod 0 is the tile the camera is standing in or adjacent to — treat it
     * as urgent so it wins admission over farther, coarser LODs contending
     * for the same frame's budget. */
    KilnStreamUrgency urg = (lod == 0) ? KILN_STREAM_URGENT : KILN_STREAM_NORMAL;
    slot->req = kiln_stream_request(&b->io->stream, key, urg, dist_sq,
                                    (uint32_t)need, slot);

    return NULL; /* resolved later by kiln_streamio_pump */
}

void kiln_streamio_tile_on_unload(int16_t tx, int16_t ty, uint8_t lod,
                                  void *user_data, void *user)
{
    (void)lod; (void)user_data;
    KilnStreamIOTileBinding *b = (KilnStreamIOTileBinding *)user;
    KilnStreamIOSlot *slot = &b->slots[tile_slot_index(b->grid, tx, ty)];

    if (slot->handle != KILN_CACHE_HANDLE_INVALID) {
        kiln_cache_release(b->io->cache, slot->handle,
                          release_fn_for(slot->asset_type), b->io->db);
        slot->handle = KILN_CACHE_HANDLE_INVALID;
    } else if (slot->req != KILN_STREAM_HANDLE_INVALID) {
        kiln_stream_cancel(&b->io->stream, slot->req);
        slot->req = KILN_STREAM_HANDLE_INVALID;
    }
}
