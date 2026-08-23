/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * libdragon DFS backend — reads a StreamDB out of the ROM filesystem.
 *
 * DFS is read-only and seekable, which is exactly the shape this reader wants.
 * Note dfs_read is a ROM DMA underneath: reads are far cheaper when aligned
 * and batched, which is why the core reads the index and trie as whole blobs
 * once at open rather than field by field.
 */
#define STREAMDB_EMB_BACKEND_DFS 1
#include "streamdb_embedded.h"

#include <libdragon.h>
#include <string.h>

typedef struct { int fd; uint64_t len; } dfs_ctx_t;

size_t streamdb_emb_io_dfs_size(void) { return sizeof(dfs_ctx_t); }

static int dfs_read_at(void *ctx, uint64_t off, void *buf, size_t len)
{
    dfs_ctx_t *d = (dfs_ctx_t *)ctx;
    if (dfs_seek(d->fd, (int)off, SEEK_SET) != DFS_ESUCCESS) return -1;
    return dfs_read(buf, 1, (int)len, d->fd) == (int)len ? 0 : -1;
}

static uint64_t dfs_size_of(void *ctx) { return ((dfs_ctx_t *)ctx)->len; }

streamdb_emb_result_t streamdb_emb_io_dfs(streamdb_emb_io_t *io,
                                          void *storage, const char *path)
{
    if (!io || !storage || !path) return STREAMDB_EMB_ERR_INVAL;
    dfs_ctx_t *d = (dfs_ctx_t *)storage;

    /* libdragon's dfs_open takes a native DFS path ("assets.streamdb"), not
     * the newlib-style "rom:/assets.streamdb" prefix kiln_asset.h's own doc
     * comment tells every caller to pass (and every caller in this repo
     * does). Strip it here so both forms work — kiln_map_load's dfs_open
     * call hit this exact mismatch once already; see its comment. Without
     * this, dfs_open fails on real DFS (console/emulator) while the host
     * stub backend used by nix/checks/kiln-asset.nix never exercises this
     * function at all, so the mismatch was invisible to every existing gate. */
    if (strncmp(path, "rom:/", 5) == 0) path += 5;

    d->fd = dfs_open(path);
    if (d->fd < 0) return STREAMDB_EMB_ERR_IO;
    d->len = (uint64_t)dfs_size(d->fd);
    io->ctx = d;
    io->read = dfs_read_at;
    io->size = dfs_size_of;
    return STREAMDB_EMB_OK;
}

void streamdb_emb_io_dfs_close(streamdb_emb_io_t *io)
{
    if (io && io->ctx) {
        dfs_ctx_t *d = (dfs_ctx_t *)io->ctx;
        if (d->fd >= 0) { dfs_close(d->fd); d->fd = -1; }
    }
}
