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

            /* The same payload, pulled through streamdb_emb_read_range in
             * awkward chunks, must reassemble byte-identically. 7 is chosen
             * because it divides nothing here: every document ends with a
             * partial chunk, which is the case a streamer hits once per file
             * and the one most likely to be off by one.
             *
             * Nested inside the CRC-verified branch on purpose. The check
             * script corrupts one payload and asserts EXACTLY ONE "checksum
             * mismatch"; a ranged read does not CRC, so running these on a
             * document that already failed would report a second, differently
             * worded failure for the same single corruption. */
            streamdb_emb_doc_t doc;
            r = streamdb_emb_find(&db, key, strlen(key), &doc);
            CHECK(r == STREAMDB_EMB_OK, "find %s: %s", key,
                  streamdb_emb_strerror(r));
            if (r == STREAMDB_EMB_OK) {
                unsigned char *asm_buf = malloc((size_t)n ? (size_t)n : 1);
                size_t at = 0;
                int range_ok = 1;
                while (at < (size_t)n) {
                    size_t want_n = 7;
                    r = streamdb_emb_read_range(&db, &doc, at,
                                                asm_buf + at, &want_n);
                    if (r != STREAMDB_EMB_OK) {
                        CHECK(0, "%s read_range at %zu: %s", key, at,
                              streamdb_emb_strerror(r));
                        range_ok = 0;
                        break;
                    }
                    /* Zero bytes with data remaining would spin forever. */
                    if (want_n == 0) {
                        CHECK(0, "%s read_range stalled at %zu/%ld",
                              key, at, n);
                        range_ok = 0;
                        break;
                    }
                    at += want_n;
                }
                if (range_ok) {
                    CHECK(at == (size_t)n, "%s ranged total %zu != %ld",
                          key, at, n);
                    CHECK(memcmp(asm_buf, want, n) == 0,
                          "%s ranged payload differs from whole read", key);
                }

                /* Reading AT the end is a legal empty read, not an error —
                 * a streamer that just consumed the last byte asks once more
                 * and must be told zero rather than handed a failure. */
                size_t tail = 64;
                r = streamdb_emb_read_range(&db, &doc, (uint64_t)n,
                                            asm_buf, &tail);
                CHECK(r == STREAMDB_EMB_OK && tail == 0,
                      "%s read_range at EOF: %s, len %zu", key,
                      streamdb_emb_strerror(r), tail);

                /* Past the end is a caller arithmetic bug and is refused. */
                tail = 64;
                r = streamdb_emb_read_range(&db, &doc, (uint64_t)n + 1,
                                            asm_buf, &tail);
                CHECK(r == STREAMDB_EMB_ERR_INVAL,
                      "%s read_range past EOF returned %s, expected INVAL",
                      key, streamdb_emb_strerror(r));

                /* An oversized request clamps to what remains. */
                size_t big = (size_t)n + 4096;
                r = streamdb_emb_read_range(&db, &doc, 0, asm_buf, &big);
                CHECK(r == STREAMDB_EMB_OK && big == (size_t)n,
                      "%s oversized read_range gave %zu, expected %ld",
                      key, big, n);

                /* The host backend is not memory-addressable, so this must
                 * report 0 rather than inventing an address. */
                uint32_t rom = 0xDEADBEEFu;
                r = streamdb_emb_doc_rom_base(&db, &doc, &rom);
                CHECK(r == STREAMDB_EMB_OK && rom == 0,
                      "%s rom_base on stdio gave %s / 0x%08x, expected OK / 0",
                      key, streamdb_emb_strerror(r), (unsigned)rom);

                free(asm_buf);
            }
            printf("  ok  %-28s %zu bytes, CRC verified, ranged read agrees\n",
                   key, len);
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
