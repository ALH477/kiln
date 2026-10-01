/* SPDX-License-Identifier: MIT
 *
 * dungeon_parity.c -- the committed Exsecutor emission still makes the chunks
 * the Exsecutor oracle says it does, and the demo's own C agrees with both.
 *
 * Four things, in order, and the order is the order they would be debugged in:
 *
 *   1. THE STREAM. exsec's tests/programs/dungeon/probatio.exsc runs a fixed
 *      scenario through the library and writes 17,840 bytes; its expected.out
 *      is written by prototypes/dungeon_oracle.py, an independent Python
 *      implementation, and is committed here as dungeon_expected.bin. This
 *      file is that scenario in C -- section for section, in the same order --
 *      driven through dungeon_x86_64.gen.c, and compared byte for byte. It is
 *      the Exsecutor repository's own certificate re-run on the unit that is
 *      actually committed in this tree, which is the claim a committed
 *      generated file rots on.
 *   2. THE GOLDEN HEADER. dungeon_golden.h is what main.c checks on the
 *      console. Every field is compared to the matching record of the stream,
 *      so a hand edit of either fails here.
 *   3. THE VIEW. dungeon_view.c reduces a chunk to rectangles. It is checked
 *      against the chunk it was built from: the runs cover exactly the
 *      non-wall tiles, never cross a row, and are maximal.
 *   4. THE INVARIANTS the oracle asserts, on chunks this file generates:
 *      tile codes, a solid border, and ONE 4-connected component -- by a flood
 *      fill written here, in C, independently of the Exsecutor prune it is
 *      checking.
 *
 * It needs no exsc and no libdragon: <stdint.h>, <stdio.h> and the two units.
 * Run it four ways (gcc and clang, -O0 and -O2, UBSan), as the Exsecutor
 * repository's differential phase does.
 *
 *     cc -std=c11 -O2 -fsanitize=undefined -fno-sanitize-recover=all \
 *        -o parity dungeon_parity.c dungeon_view.c dungeon_x86_64.gen.c
 *     ./parity dungeon_expected.bin
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "exsec_dungeon.h"
#include "dungeon_view.h"
#include "dungeon_golden.h"

_Noreturn void exsrt_abortus(unsigned kind)
{
    fprintf(stderr, "exsecutor: abortus %u\n", kind);
    exit(70);
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the stream, as probatio.exsc writes it ------------------------------ */

#define STREAM_MAX 20000
static unsigned char g_stream[STREAM_MAX];
static size_t g_len;

static void put_u64(uint64_t v)
{
    for (int i = 7; i >= 0; i--) g_stream[g_len++] = (unsigned char)(v >> (8 * i));
}

static unsigned char g_work[EXSEC_DUNGEON_WORK_BYTES];

static void put_chunk(uint64_t seed, int raw)
{
    uint64_t st[3] = { 0, 0, 0 };
    const uint64_t walk = exs_fig_dungeon_chunk(g_work, (unsigned char *)st, seed);
    put_u64(seed);
    put_u64(walk);
    put_u64(st[0]);
    put_u64(st[1]);
    put_u64(st[2]);
    put_u64(exs_fig_dungeon_crc32(g_work) & 0xFFFFFFFFu);
    if (raw) { memcpy(g_stream + g_len, g_work, EXSEC_DUNGEON_TILES); g_len += EXSEC_DUNGEON_TILES; }
}

/* The scenario. Keep it identical to examples/dungeon/probatio.exsc and to
 * prototypes/dungeon_oracle.py's `stream`: WORLDS, INDICES, SEEDS_EXTRA,
 * CHUNKS_RAW, DIGEST_WORLD, DIGEST_RANGE and `synthetic`. */
static const uint64_t WORLDS[2]   = { 0, UINT64_C(0xDEADBEEFCAFEF00D) };
static const uint64_t INDICES[9]  = { 0, 1, 2, 3, 7, 64, 4095, UINT64_C(244140624999),
                                      UINT64_C(0xFFFFFFFFFFFFFFFF) };
/* Direct seeds. The last two are chunks where the prune FIRES (it removes 1 tile
 * and 5), found by scanning 2,000,000 chunks of world 0: a comparison of seeds
 * that never hits the prune cannot test it. */
#define EXTRA_COUNT 4
static const uint64_t SEEDS_EXTRA[EXTRA_COUNT] = {
    UINT64_C(0xFFFFFFFFFFFFFFFF), 1, UINT64_C(0xF8536F0B5739D6EC), UINT64_C(0xA8B1E4B207C1C4C8)
};

static void build_stream(void)
{
    g_len = 0;
    /* A. the petabyte arithmetic */
    const uint64_t pb = UINT64_C(1000000000000000);
    const uint64_t chunks = pb / 4096;
    put_u64(chunks);
    put_u64(pb % 4096);
    put_u64(UINT64_C(0xFFFFFFFFFFFFFFFF) / chunks);
    put_u64(UINT64_C(0xFFFFFFFFFFFFFFFF) > chunks ? 1 : 0);
    put_u64(UINT64_C(4294967296) < chunks ? 1 : 0);
    /* B. the seed function */
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 9; b++) put_u64(exs_semina_furore(WORLDS[a], INDICES[b]));
    /* C. chunks: every (world, index), then the direct seeds; raw bytes for the first four */
    int raw = 0;
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 9; b++) {
            put_chunk(exs_semina_furore(WORLDS[a], INDICES[b]), raw < 4);
            raw++;
        }
    for (unsigned i = 0; i < sizeof SEEDS_EXTRA / sizeof SEEDS_EXTRA[0]; i++)
        put_chunk(SEEDS_EXTRA[i], 0);
    /* D. a rolled digest over 256 chunks of one world */
    uint64_t acc = 0;
    for (uint64_t i = 0; i < 256; i++) {
        uint64_t st[3];
        exs_fig_dungeon_chunk(g_work, (unsigned char *)st,
                              exs_semina_furore(UINT64_C(0xDEADBEEFCAFEF00D), i));
        acc = ((acc << 7) | (acc >> 57)) ^ (exs_fig_dungeon_crc32(g_work) & 0xFFFFFFFFu);
    }
    put_u64(256);
    put_u64(acc);
    /* E. the prune on a grid with islands in it */
    memset(g_work, 0, sizeof g_work);
    for (int y = 10; y < 13; y++) for (int x = 10; x < 21; x++) g_work[y * 64 + x] = 1;
    for (int x = 21; x < 37; x++) g_work[11 * 64 + x] = 1;
    for (int y = 40; y < 43; y++) for (int x = 40; x < 43; x++) g_work[y * 64 + x] = 1;
    g_work[5 * 64 + 50] = 1;
    g_work[12 * 64 + 37] = 1;
    const uint64_t pruned = exs_fig_dungeon_prune(g_work, 11 * 64 + 11);
    put_u64(pruned);
    put_u64(exs_fig_dungeon_crc32(g_work) & 0xFFFFFFFFu);
    put_u64(exs_fig_dungeon_prune(g_work, 0));
    /* F. the seed's inverse: the index back from each of the eighteen seeds, then
     * the indices per world whose seed is 0 and whose seed is GAMMA */
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 9; b++)
            put_u64(exs_desemina_furore(exs_semina_furore(WORLDS[a], INDICES[b]), WORLDS[a]));
    for (int a = 0; a < 2; a++) {
        put_u64(exs_desemina_furore(0, WORLDS[a]));
        put_u64(exs_desemina_furore(UINT64_C(0x9E3779B97F4A7C15), WORLDS[a]));
    }
}

/* ---- 2. the golden header against the stream ---------------------------- */

static uint64_t be64(const unsigned char *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static void check_golden(const unsigned char *s, size_t n)
{
    for (int i = 0; i < DUNGEON_GOLDEN_COUNT; i++) {
        const DungeonGolden *g = &DUNGEON_GOLDEN[i];
        const uint64_t seed = exs_semina_furore(g->world, g->index);
        CHECK(seed == g->seed, "golden %d: seed %016llx != %016llx", i,
              (unsigned long long)seed, (unsigned long long)g->seed);
        /* find the record whose first word is this seed. Section C starts at
         * byte 184 (five words, then eighteen seeds); record k is 48 bytes and
         * the first four are each followed by 4,096 raw bytes. */
        int found = 0;
        for (size_t k = 0; ; k++) {
            const size_t off = 184 + k * 48 + (k < 4 ? k : 4) * EXSEC_DUNGEON_TILES;
            if (off + 48 > n || k >= 18 + EXTRA_COUNT) break;
            if (be64(s + off) == g->seed) {
                found = 1;
                CHECK(be64(s + off + 8)  == g->walkable, "golden %d: walkable", i);
                CHECK(be64(s + off + 16) == g->draws,    "golden %d: draws", i);
                CHECK(be64(s + off + 24) == g->boundary, "golden %d: boundary", i);
                CHECK(be64(s + off + 32) == g->pruned,   "golden %d: pruned", i);
                CHECK(be64(s + off + 40) == g->crc,      "golden %d: crc", i);
                break;
            }
        }
        CHECK(found, "golden %d: no record with seed %016llx in the stream", i,
              (unsigned long long)g->seed);
    }
}

/* ---- 3. the view --------------------------------------------------------- */

static void check_view(const unsigned char *chunk, const char *what)
{
    static DungeonRun runs[DUNGEON_VIEW_MAX_RUNS];
    const int n = dungeon_view_runs(chunk, runs);
    CHECK(n > 0 && n <= DUNGEON_VIEW_MAX_RUNS, "%s: %d runs", what, n);
    unsigned char painted[EXSEC_DUNGEON_TILES];
    memset(painted, 0, sizeof painted);
    for (int i = 0; i < n; i++) {
        const DungeonRun r = runs[i];
        CHECK(r.kind >= 1 && r.kind <= 3, "%s: run %d kind %d", what, i, r.kind);
        CHECK(r.w >= 1 && r.x + r.w <= 64 && r.y < 64, "%s: run %d is off the grid or crosses a row", what, i);
        for (int k = 0; k < r.w && r.x + k < 64; k++) {
            const int t = r.y * 64 + r.x + k;
            CHECK(painted[t] == 0, "%s: tile %d painted twice", what, t);
            painted[t] = r.kind;
        }
        /* maximal: the next tile in the row must not continue the run */
        if (r.x + r.w < 64)
            CHECK(chunk[r.y * 64 + r.x + r.w] != r.kind, "%s: run %d is not maximal", what, i);
    }
    CHECK(memcmp(painted, chunk, EXSEC_DUNGEON_TILES) == 0,
          "%s: the runs do not reproduce the chunk", what);
    uint32_t c[4];
    dungeon_view_census(chunk, c);
    CHECK(c[0] + c[1] + c[2] + c[3] == EXSEC_DUNGEON_TILES, "%s: census does not sum to 4096", what);
    uint32_t nonwall = 0;
    for (int t = 0; t < EXSEC_DUNGEON_TILES; t++) nonwall += chunk[t] != 0;
    CHECK(c[1] + c[2] + c[3] == nonwall, "%s: census disagrees with a count", what);
}

/* ---- 4. invariants, by a flood fill that is not the unit's --------------- */

static int components(const unsigned char *g)
{
    static unsigned char seen[EXSEC_DUNGEON_TILES];
    static int stack[EXSEC_DUNGEON_TILES];
    memset(seen, 0, sizeof seen);
    int comps = 0;
    for (int s = 0; s < EXSEC_DUNGEON_TILES; s++) {
        if (!g[s] || seen[s]) continue;
        comps++;
        int sp = 0;
        stack[sp++] = s;
        seen[s] = 1;
        while (sp) {
            const int u = stack[--sp];
            const int x = u % 64, y = u / 64;
            const int nb[4] = { y > 0 ? u - 64 : -1, y < 63 ? u + 64 : -1,
                                x > 0 ? u - 1 : -1,  x < 63 ? u + 1 : -1 };
            for (int k = 0; k < 4; k++)
                if (nb[k] >= 0 && g[nb[k]] && !seen[nb[k]]) { seen[nb[k]] = 1; stack[sp++] = nb[k]; }
        }
    }
    return comps;
}

static void check_invariants(uint64_t seed)
{
    uint64_t st[3];
    memset(g_work, 0xEE, sizeof g_work);
    const uint64_t walk = exs_fig_dungeon_chunk(g_work, (unsigned char *)st, seed);
    uint64_t count = 0;
    for (int t = 0; t < EXSEC_DUNGEON_TILES; t++) {
        CHECK(g_work[t] <= 3, "seed %016llx: tile %d is %d", (unsigned long long)seed, t, g_work[t]);
        count += g_work[t] != 0;
    }
    CHECK(count == walk, "seed %016llx: returned %llu walkable, counted %llu",
          (unsigned long long)seed, (unsigned long long)walk, (unsigned long long)count);
    for (int i = 0; i < 64; i++)
        CHECK(!g_work[i] && !g_work[63 * 64 + i] && !g_work[i * 64] && !g_work[i * 64 + 63],
              "seed %016llx: the border is not solid at %d", (unsigned long long)seed, i);
    CHECK(components(g_work) == 1, "seed %016llx: %d components", (unsigned long long)seed,
          components(g_work));
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s dungeon_expected.bin\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    static unsigned char want[STREAM_MAX];
    const size_t wn = fread(want, 1, sizeof want, f);
    fclose(f);

    printf("mixer: mix(GAMMA) = %016llx\n",
           (unsigned long long)exs_misce_furore(UINT64_C(0x9E3779B97F4A7C15)));
    CHECK(exs_misce_furore(UINT64_C(0x9E3779B97F4A7C15)) == UINT64_C(0xE220A8397B1DCDAF),
          "the mixer does not reproduce splitmix64's published first output");

    build_stream();
    printf("stream: %zu bytes built, %zu committed\n", g_len, wn);
    CHECK(g_len == wn, "length %zu != %zu", g_len, wn);
    if (memcmp(g_stream, want, g_len < wn ? g_len : wn) != 0) {
        size_t i = 0;
        while (i < g_len && i < wn && g_stream[i] == want[i]) i++;
        CHECK(0, "the stream differs from the oracle's at byte %zu (of %zu)", i, wn);
    } else if (g_len == wn) {
        printf("stream: byte-identical to the oracle's expected.out\n");
    }

    /* the seed module's bijection, on inputs the scenario does not name: a
     * walk of indices, including the extremes, in three worlds */
    {
        static const uint64_t ws[3] = { 0, UINT64_C(0xDEADBEEFCAFEF00D), UINT64_C(0xFFFFFFFFFFFFFFFF) };
        int rt = 0;
        for (int a = 0; a < 3; a++)
            for (uint64_t i = 0; i < 4000; i++) {
                const uint64_t ix = i * UINT64_C(0x9E3779B97F4A7C15) + i;   /* spread over 2^64 */
                CHECK(exs_desemina_furore(exs_semina_furore(ws[a], ix), ws[a]) == ix,
                      "round trip fails: world %016llx index %016llx",
                      (unsigned long long)ws[a], (unsigned long long)ix);
                rt++;
            }
        CHECK(exs_demisce_furore(exs_misce_furore(UINT64_C(0xFFFFFFFFFFFFFFFF))) ==
              UINT64_C(0xFFFFFFFFFFFFFFFF), "demisce(misce(2^64-1))");
        printf("seed: %d (world, index) round trips through desemina_furore, indices spread over 2^64\n", rt);
    }

    check_golden(want, wn);
    printf("golden: %d vectors checked against the stream\n", DUNGEON_GOLDEN_COUNT);

    uint64_t st[3];
    exs_fig_dungeon_chunk(g_work, (unsigned char *)st, DUNGEON_GOLDEN[0].seed);
    check_view(g_work, "golden 0");
    exs_fig_dungeon_chunk(g_work, (unsigned char *)st, SEEDS_EXTRA[3]);
    check_view(g_work, "a chunk the prune trimmed");
    memset(g_work, 3, EXSEC_DUNGEON_TILES);            /* all traps: one run a row, 64 of them */
    check_view(g_work, "all traps");
    memset(g_work, 0, EXSEC_DUNGEON_TILES);
    CHECK(dungeon_view_runs(g_work, (DungeonRun[1]){ 0 }) == 0, "a solid chunk has no runs");
    printf("view: runs reproduce chunks, are maximal, cross no row\n");

    int checked = 0;
    for (uint64_t i = 0; i < 300; i++, checked++)
        check_invariants(exs_semina_furore(UINT64_C(0xDEADBEEFCAFEF00D), i));
    for (unsigned i = 0; i < sizeof SEEDS_EXTRA / sizeof SEEDS_EXTRA[0]; i++, checked++)
        check_invariants(SEEDS_EXTRA[i]);
    printf("invariants: %d chunks: codes 0..3, solid border, one connected component\n", checked);

    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("dungeon parity: ok\n");
    return 0;
}
