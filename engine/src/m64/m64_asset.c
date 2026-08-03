/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_asset.c — runtime asset layer. See m64_asset.h for the model.
 *
 * A thin shell over streamdb-embedded plus libdragon's sprite_load_buf and
 * the patched t3d_model_load_buf. The shape is: read the payload into a
 * malloc'd buffer with streamdb_emb_get (which CRC-verifies it), then hand
 * the buffer to the right in-memory parser.
 */

#include "m64_asset.h"

#include <malloc.h>
#include <string.h>

/* The handle IS the streamdb_emb_t by value; one allocation holds the
 * reader plus the caller's arena pointer. Kept opaque in the header so a
 * ROM can stack-allocate the streamdb_emb_t directly if it prefers — but
 * the convenience API here is the one the demo uses. */
struct M64Asset {
    streamdb_emb_t db;
    int open;
};

/* One static reader is enough for an N64 ROM: there is exactly one asset DB
 * per ROM, mounted at boot, unmounted at shutdown. If a game ever wants two
 * DBs (e.g. a level pack swapped mid-game) this becomes a small pool; until
 * then a single static is honest about the actual usage and free. */
static struct M64Asset g_db;

static streamdb_emb_io_t io_make_dfs(const char *path, void *storage)
{
    streamdb_emb_io_t io;
    streamdb_emb_result_t r = streamdb_emb_io_dfs(&io, storage, path);
    if (r != STREAMDB_EMB_OK) {
        io.ctx = NULL;
        io.read = NULL;
        io.size = NULL;
    }
    return io;
}

size_t m64_asset_probe_size(const char *dfs_path)
{
    if (!dfs_path) return 0;
    /* The DFS backend's storage is small (a fd + a length); stack-allocate
     * it for the probe, then close. */
    char storage[64];
    streamdb_emb_io_t io = io_make_dfs(dfs_path, storage);
    if (!io.ctx) return 0;

    size_t need = 0;
    streamdb_emb_result_t r = streamdb_emb_probe(&io, &need);
    streamdb_emb_io_dfs_close(&io);
    return r == STREAMDB_EMB_OK ? need : 0;
}

M64Asset *m64_asset_open(const char *dfs_path, void *arena, size_t arena_size)
{
    if (!dfs_path || !arena || arena_size == 0) return NULL;

    /* The DFS backend's storage lives inside the reader handle so the io
     * struct's lifetime matches the DB's. Sized by streamdb_io_dfs_size. */
    static char io_storage[64];
    size_t need = streamdb_emb_io_dfs_size();
    if (need > sizeof(io_storage)) return NULL;

    streamdb_emb_io_t io = io_make_dfs(dfs_path, io_storage);
    if (!io.ctx) return NULL;

    streamdb_emb_result_t r = streamdb_emb_open(&g_db.db, &io, arena, arena_size);
    if (r != STREAMDB_EMB_OK) {
        streamdb_emb_io_dfs_close(&io);
        return NULL;
    }
    g_db.open = 1;
    return &g_db;
}

void m64_asset_close(M64Asset *db)
{
    if (!db || !db->open) return;
    streamdb_emb_io_dfs_close(&db->db.io);
    db->open = 0;
}

uint32_t m64_asset_count(const M64Asset *db)
{
    return db ? streamdb_emb_count(&db->db) : 0u;
}

size_t m64_asset_size(const M64Asset *db, const char *key, size_t key_len)
{
    if (!db || !key || !key_len) return 0;
    streamdb_emb_doc_t doc;
    if (streamdb_emb_find(&db->db, key, key_len, &doc) != STREAMDB_EMB_OK) {
        return 0;
    }
    return doc.size;
}

int m64_asset_exists(const M64Asset *db, const char *key, size_t key_len)
{
    if (!db || !key || !key_len) return 0;
    streamdb_emb_doc_t doc;
    return streamdb_emb_find(&db->db, key, key_len, &doc) == STREAMDB_EMB_OK;
}

int m64_asset_load(const M64Asset *db,
                   const char *key, size_t key_len,
                   void *buf, size_t *len)
{
    if (!db || !key || !key_len || !buf || !len) return STREAMDB_EMB_ERR_INVAL;
    return streamdb_emb_get(&db->db, key, key_len, buf, len);
}

int m64_asset_find_suffix(const M64Asset *db,
                          const char *suffix, size_t suffix_len,
                          int (*cb)(const streamdb_emb_doc_t *doc, void *user),
                          void *user)
{
    if (!db || !suffix || !suffix_len) return 0;
    return streamdb_emb_find_suffix(&db->db, suffix, suffix_len, cb, user);
}

sprite_t *m64_asset_sprite(const M64Asset *db, const char *key, size_t key_len)
{
    if (!db || !key || !key_len) return NULL;

    size_t len = m64_asset_size(db, key, key_len);
    if (len == 0) return NULL;

    void *buf = malloc(len);
    if (!buf) return NULL;

    size_t got = len;
    if (m64_asset_load(db, key, key_len, buf, &got) != STREAMDB_EMB_OK) {
        free(buf);
        return NULL;
    }
    sprite_t *sp = sprite_load_buf(buf, (int)got);
    if (!sp) {
        free(buf);
        return NULL;
    }
    /* Mirror sprite_load's ownership transfer so the caller frees with plain
     * sprite_free. In the usual case sprite_load_buf returns `buf` itself
     * (parsed in place) and we mark the buffer owned by the sprite; a
     * custom decoder (e.g. LSPR) returns a fresh sprite_t and the source
     * buffer is now redundant. See libdragon's sprite.c. */
    if ((void *)sp == buf) {
        sp->flags |= SPRITE_FLAGS_OWNEDBUFFER;
    } else {
        free(buf);
    }
    return sp;
}

T3DModel *m64_asset_model(const M64Asset *db, const char *key, size_t key_len)
{
    if (!db || !key || !key_len) return NULL;

    size_t len = m64_asset_size(db, key, key_len);
    if (len == 0) return NULL;

    /* t3d_model_load_buf also parses in place, so the buffer must outlive
     * the T3DModel. t3d_model_free frees the model pointer, which IS the
     * buffer (the buffer was returned by asset_load upstream, and we're
     * handing it the same shape), so freeing the model frees both. */
    void *buf = malloc(len);
    if (!buf) return NULL;

    size_t got = len;
    if (m64_asset_load(db, key, key_len, buf, &got) != STREAMDB_EMB_OK) {
        free(buf);
        return NULL;
    }
    T3DModel *m = t3d_model_load_buf(buf, (int)got);
    if (!m) {
        free(buf);
        return NULL;
    }
    return m;
}