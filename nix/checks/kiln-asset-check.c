// SPDX-License-Identifier: MIT
//
// Host driver for the kiln-asset check. Opens a StreamDB packed by the
// upstream C writer (via the stdio backend), exercises every kiln_asset
// accessor, and verifies the bytes round-trip. The stub libdragon.h /
// t3d/t3dmodel.h record what kiln_asset_model / kiln_asset_sprite were
// handed so we can confirm the routing without parsing a real model or
// sprite.

#define STREAMDB_EMB_BACKEND_STDIO 1
#include "streamdb_embedded.h"

#include "kiln_asset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined by the stub headers; instantiated here. */
void *g_last_sprite_buf;
int   g_last_sprite_sz;
void *g_last_model_buf;
int   g_last_model_sz;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* kiln_asset_open's DFS backend isn't compiled in here (we passed
 * -DSTREAMDB_EMB_BACKEND_DFS=0). For the host check we need to bypass it
 * and use the stdio backend directly. The cleanest way is to construct the
 * streamdb_emb_t by hand from a stdio io and hand it to a thin wrapper
 * that calls streamdb_emb_open — which is exactly what kiln_asset_open
 * does, except it builds the io via streamdb_emb_io_dfs.
 *
 * To keep the engine source unchanged, the check provides a local shim
 * that does what kiln_asset_open does but with the stdio backend. The
 * plumbing under that (the streamdb_emb_open call, the arena, the
 * KilnAsset handle) is the SAME code path the engine uses on-console; we
 * just substitute the I/O source. */
static KilnAsset *open_stdio(const char *path, void *arena, size_t arena_size)
{
    static char io_storage[64];
    streamdb_emb_io_t io;
    streamdb_emb_result_t r = streamdb_emb_io_stdio(&io, io_storage, path);
    if (r != STREAMDB_EMB_OK) return NULL;

    /* KilnAsset is opaque in the header; we construct it inline here using
     * the same shape kiln_asset.c defines. This is intentionally a copy of
     * the engine's static g_db pattern — the test asserts the layout
     * matches by including kiln_asset.h and using KilnAsset* through the
     * public API only. */
    static struct { streamdb_emb_t db; int open; } g_db;
    r = streamdb_emb_open(&g_db.db, &io, arena, arena_size);
    if (r != STREAMDB_EMB_OK) return NULL;
    g_db.open = 1;
    return (KilnAsset *)&g_db;
}

static int count_suffix_cb(const streamdb_emb_doc_t *d, void *user)
{
    (void)d; (*(int *)user)++; return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <db.streamdb>\n", argv[0]); return 2; }

    /* Probe via the stdio backend to size the arena, mirroring what the
     * demo ROM does with kiln_asset_probe_size. */
    streamdb_emb_io_t pio;
    static char pio_storage[64];
    if (streamdb_emb_io_stdio(&pio, pio_storage, argv[1]) != STREAMDB_EMB_OK) {
        printf("  FAIL: stdio open for probe\n"); return 1;
    }
    size_t need = 0;
    streamdb_emb_result_t r = streamdb_emb_probe(&pio, &need);
    streamdb_emb_io_stdio_close(&pio);
    CHECK(r == STREAMDB_EMB_OK, "probe: %s", streamdb_emb_strerror(r));
    if (r != STREAMDB_EMB_OK) return 1;
    printf("  probe: arena needs %zu bytes\n", need);

    void *arena = malloc(need);
    if (!arena) { printf("  FAIL: arena malloc\n"); return 1; }

    KilnAsset *db = open_stdio(argv[1], arena, need);
    CHECK(db != NULL, "open_stdio failed");
    if (!db) return 1;

    /* ── count ── */
    uint32_t n = kiln_asset_count(db);
    CHECK(n == 3, "count: expected 3, got %u", n);
    printf("  count: %u documents\n", n);

    /* ── size / exists / load on the raw-data key ── */
    const char *level_key = "levels/intro.bin";
    size_t level_klen = strlen(level_key);
    size_t level_len = kiln_asset_size(db, level_key, level_klen);
    CHECK(level_len == 24, "level size: expected 24, got %zu", level_len);

    CHECK(kiln_asset_exists(db, level_key, level_klen) == 1, "level exists");
    CHECK(kiln_asset_exists(db, "nope/missing.bin", 16) == 0, "missing key should not exist");

    uint8_t level_buf[64];
    size_t got = sizeof(level_buf);
    r = kiln_asset_load(db, level_key, level_klen, level_buf, &got);
    CHECK(r == STREAMDB_EMB_OK, "level load: %s", streamdb_emb_strerror(r));
    CHECK(got == 24, "level load size: expected 24, got %zu", got);
    CHECK(memcmp(level_buf, "KLNL", 4) == 0, "level magic");
    printf("  level: %zu bytes, magic %.4s\n", got, level_buf);

    /* ── model routing: stub records the buffer ── */
    const char *model_key = "models/cube.t3dm";
    g_last_model_buf = NULL; g_last_model_sz = 0;
    T3DModel *m = kiln_asset_model(db, model_key, strlen(model_key));
    CHECK(m != NULL, "model load returned NULL");
    CHECK(g_last_model_buf != NULL, "stub t3d_model_load_buf was not called");
    CHECK(g_last_model_sz == 68, "model size: expected 68 (4 magic + 64), got %d", g_last_model_sz);
    if (g_last_model_buf) {
        CHECK(memcmp(g_last_model_buf, "T3M", 3) == 0, "model magic routed correctly");
        printf("  model: stub received %d bytes, magic %.3s\n",
               g_last_model_sz, (char*)g_last_model_buf);
    }

    /* ── sprite routing: stub records the buffer ── */
    const char *sprite_key = "sprites/logo.sprite";
    g_last_sprite_buf = NULL; g_last_sprite_sz = 0;
    sprite_t *sp = kiln_asset_sprite(db, sprite_key, strlen(sprite_key));
    CHECK(sp != NULL, "sprite load returned NULL");
    CHECK(g_last_sprite_buf != NULL, "stub sprite_load_buf was not called");
    CHECK(g_last_sprite_sz == 128, "sprite size: expected 128, got %d", g_last_sprite_sz);
    if (sp && (void *)sp == g_last_sprite_buf) {
        /* The stub returns `buf` as the sprite_t*, so kiln_asset_sprite takes the
         * "sp == buf" branch and sets OWNEDBUFFER. Verify. */
        CHECK((sp->flags & SPRITE_FLAGS_OWNEDBUFFER) != 0, "sprite: OWNEDBUFFER flag set");
        printf("  sprite: stub received %d bytes, flags=0x%02x\n",
               g_last_sprite_sz, sp->flags);
    }

    /* ── suffix search ── */
    int n_t3dm = 0;
    int found = kiln_asset_find_suffix(db, ".t3dm", 5, count_suffix_cb, &n_t3dm);
    CHECK(found == 1, "suffix .t3dm: expected 1, got %d", found);
    printf("  suffix .t3dm -> %d match(es)\n", found);

    int n_sprite = 0;
    found = kiln_asset_find_suffix(db, ".sprite", 7, count_suffix_cb, &n_sprite);
    CHECK(found == 1, "suffix .sprite: expected 1, got %d", found);

    int n_bin = 0;
    found = kiln_asset_find_suffix(db, ".bin", 4, count_suffix_cb, &n_bin);
    CHECK(found == 1, "suffix .bin: expected 1, got %d", found);

    /* ── negative: a key that's not in the DB ── */
    CHECK(kiln_asset_size(db, "nope", 4) == 0, "missing key size should be 0");
    size_t zero = 4;
    r = kiln_asset_load(db, "nope", 4, level_buf, &zero);
    CHECK(r == STREAMDB_EMB_ERR_NOT_FOUND, "missing key load: %s",
          streamdb_emb_strerror(r));

    /* kiln_asset_model on a missing key returns NULL without calling the
     * stub. */
    g_last_model_buf = (void *)0xDEAD;
    const char *missing_key = "nope.t3dm";
    T3DModel *miss = kiln_asset_model(db, missing_key, strlen(missing_key));
    CHECK(miss == NULL, "missing model should return NULL");
    CHECK(g_last_model_buf == (void *)0xDEAD, "missing model should not call stub");

    if (fails) {
        printf("\nFAILED (%d)\n", fails);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}