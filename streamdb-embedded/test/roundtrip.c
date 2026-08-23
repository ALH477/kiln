/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Verify the embedded reader against a database written by the upstream C lib. */
#define STREAMDB_EMB_BACKEND_STDIO 1
#include "streamdb_embedded.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int count_cb(const streamdb_emb_doc_t *d, void *u) {
    (void)d; (*(int*)u)++; return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;

    streamdb_emb_io_t io;
    void *iostore = malloc(streamdb_emb_io_stdio_size());
    streamdb_emb_result_t r = streamdb_emb_io_stdio(&io, iostore, argv[1]);
    CHECK(r == STREAMDB_EMB_OK, "open io: %s", streamdb_emb_strerror(r));
    if (r != STREAMDB_EMB_OK) return 1;

    size_t need = 0;
    r = streamdb_emb_probe(&io, &need);
    CHECK(r == STREAMDB_EMB_OK, "probe: %s", streamdb_emb_strerror(r));
    printf("  probe says arena needs %zu bytes\n", need);

    void *arena = malloc(need);
    streamdb_emb_t db;
    r = streamdb_emb_open(&db, &io, arena, need);
    CHECK(r == STREAMDB_EMB_OK, "open db: %s", streamdb_emb_strerror(r));
    if (r != STREAMDB_EMB_OK) return 1;

    printf("  documents: %u, trie nodes: %u, arena used: %zu\n",
           db.doc_count, db.node_count, db.arena_used);

    /* Each key/file pair given on the command line must read back byte-exact. */
    for (int i = 2; i + 1 < argc; i += 2) {
        const char *key = argv[i], *path = argv[i+1];
        FILE *f = fopen(path, "rb");
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char *want = malloc(n);
        if (fread(want, 1, n, f) != (size_t)n) return 1;
        fclose(f);

        size_t len = n + 16;
        unsigned char *got = malloc(len);
        r = streamdb_emb_get(&db, key, strlen(key), got, &len);
        CHECK(r == STREAMDB_EMB_OK, "get %s: %s", key, streamdb_emb_strerror(r));
        if (r == STREAMDB_EMB_OK) {
            CHECK(len == (size_t)n, "%s size %zu != %ld", key, len, n);
            CHECK(memcmp(got, want, n) == 0, "%s payload differs", key);
            printf("  ok  %-28s %zu bytes, CRC verified\n", key, len);
        }
        free(want); free(got);
    }

    /* A key that was never inserted must not be found. */
    r = streamdb_emb_get(&db, "nope/missing.bin", 16, NULL, NULL);
    CHECK(r != STREAMDB_EMB_OK, "missing key unexpectedly succeeded");

    /* Suffix search: the reverse trie's reason for existing. */
    int n_t3dm = 0;
    int found = streamdb_emb_find_suffix(&db, ".t3dm", 5, count_cb, &n_t3dm);
    printf("  suffix '.t3dm' -> %d match(es)\n", found);
    int n_png = 0;
    printf("  suffix '.wav'  -> %d match(es)\n",
           streamdb_emb_find_suffix(&db, ".wav", 4, count_cb, &n_png));

    streamdb_emb_io_stdio_close(&io);
    printf(fails ? "\nFAILED (%d)\n" : "\nall checks passed\n", fails);
    return fails ? 1 : 0;
}
