/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Host stdio backend. Exists so the reader can be exercised on a workstation
 * against files produced by the real Rust/C editions — the N64 has no way to
 * tell you *why* a parse failed, so every format bug should be caught here.
 */
#define STREAMDB_EMB_BACKEND_STDIO 1
#include "streamdb_embedded.h"

#include <stdio.h>

typedef struct { FILE *f; uint64_t len; } stdio_ctx_t;

size_t streamdb_emb_io_stdio_size(void) { return sizeof(stdio_ctx_t); }

static int stdio_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    stdio_ctx_t *s = (stdio_ctx_t *)ctx;
    if (fseeko(s->f, (off_t)off, SEEK_SET) != 0) return -1;
    return fread(buf, 1, len, s->f) == len ? 0 : -1;
}

static uint64_t stdio_size(void *ctx) { return ((stdio_ctx_t *)ctx)->len; }

streamdb_emb_result_t streamdb_emb_io_stdio(streamdb_emb_io_t *io,
                                            void *storage, const char *path)
{
    if (!io || !storage || !path) return STREAMDB_EMB_ERR_INVAL;
    stdio_ctx_t *s = (stdio_ctx_t *)storage;
    s->f = fopen(path, "rb");
    if (!s->f) return STREAMDB_EMB_ERR_IO;
    if (fseeko(s->f, 0, SEEK_END) != 0) { fclose(s->f); return STREAMDB_EMB_ERR_IO; }
    s->len = (uint64_t)ftello(s->f);
    io->ctx = s;
    io->read = stdio_read;
    io->size = stdio_size;
    return STREAMDB_EMB_OK;
}

void streamdb_emb_io_stdio_close(streamdb_emb_io_t *io)
{
    if (io && io->ctx) {
        stdio_ctx_t *s = (stdio_ctx_t *)io->ctx;
        if (s->f) { fclose(s->f); s->f = NULL; }
    }
}
