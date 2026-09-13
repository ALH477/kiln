// SPDX-License-Identifier: MIT
//
// A StreamDB v3 reader written in Exsecutor, running on the N64, checked
// against Kiln's own reader on the same container.
//
// WHAT THIS ROM SHOWS, line by line on screen:
//
//   1. The container opens through the Exsecutor reader: header slot chosen,
//      both CRCs verified, index well-formed, trie flattened -- the same
//      sequence examples/streamdb/probatio.exsc runs in the Exsecutor repo,
//      where it is certified byte-for-byte against the upstream C reader.
//   2. It all runs on a kthread whose stack is 32,768 bytes. The reader's
//      traversal frame is 20,680 bytes (ADR 0016); before that work it was
//      127,184, which would not have fit this thread -- nor libdragon's 64 KB
//      main stack.
//   3. Each key is looked up by BOTH readers, and the ROM prints AGREE only if
//      they find the same documents with the same sizes and the same bytes.
//      One key is deliberately absent, so "not found" is compared too.
//
// The container is flake.nix's `exsecStreamdb`, packed by the upstream C writer
// like every other .streamdb in this repo.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_asset.h>

#include <malloc.h>
#include <string.h>

#include "exsec_streamdb.h"

#define DB_PATH      "rom:/exsec.streamdb"
#define EXSEC_STACK  32768
#define NKEYS        3

static const char *const KEYS[NKEYS] = {
    "levels/intro.bin",
    "data/ave.txt",
    "data/absens.txt",   // not in the container, on purpose
};

// Static, and therefore in .bss and ZERO before main runs. That is not a
// convenience: `exs_arbor_percurre` requires its Arbor zeroed (ADR 0016
// decision 4), because it reads back slots it never writes.
static unsigned char g_buf[EXSEC_CAPACITY];
static unsigned char g_arbor[EXSEC_ARBOR_BYTES];
static unsigned char g_clavis[EXSEC_CLAVIS_MAXIMA];
static unsigned char g_caput[EXSEC_CAPUT_BYTES];
static unsigned char g_indicium[EXSEC_INDICIUM_BYTES];
static uint64_t g_len;

typedef struct {
    int      verdict;   // 0 opened; else probatio.exsc's 1-6
    uint64_t docs;
    uint64_t nodes;
    struct {
        int      state; // 1 found and CRC-valid, 0 absent, <0 -proba verdict
        uint32_t size;
        uint64_t offset;
        int      agree;
    } k[NKEYS];
} ExsecResult;

// The generated unit's one import. A trap inside Exsecutor code -- an overflow,
// a failed bounds check -- lands here with its abort kind.
_Noreturn void exsrt_abortus(unsigned kind)
{
    assertf(0, "exsecutor: abortus %u", kind);
    for (;;) { }
}

// Runs on the 32 KB thread. Everything it touches is static, so the only stack
// it uses is the reader's own frames.
static int lector(void *arg)
{
    ExsecResult *r = arg;

    uint64_t slot = exs_caput_elige(g_buf, g_len);
    if (slot >= 256) { r->verdict = 1; return 0; }
    exs_caput_lege(g_caput, g_buf, slot);

    uint64_t is = exsec_le64(g_caput + EXSEC_CAPUT_INDEX_SITUS);
    uint64_t il = exsec_le64(g_caput + EXSEC_CAPUT_INDEX_LONGITUDO);
    uint64_t as = exsec_le64(g_caput + EXSEC_CAPUT_ARBOR_SITUS);
    uint64_t al = exsec_le64(g_caput + EXSEC_CAPUT_ARBOR_LONGITUDO);

    if (exs_redundantia32(g_buf, is, il) != exsec_le32(g_caput + EXSEC_CAPUT_INDEX_SUMMA)) {
        r->verdict = 2; return 0;
    }
    if (exs_redundantia32(g_buf, as, al) != exsec_le32(g_caput + EXSEC_CAPUT_ARBOR_SUMMA)) {
        r->verdict = 3; return 0;
    }
    uint64_t n = exs_indicem_numera(g_buf, is, il);
    if (n >= EXSEC_CAPACITY) { r->verdict = 4; return 0; }
    if (exs_indicem_proba(g_buf, is, n, g_len) != 0) { r->verdict = 6; return 0; }

    r->nodes = exs_arbor_percurre(g_arbor, g_buf, as, al);
    if (r->nodes == 0) { r->verdict = 5; return 0; }
    r->docs = n;

    for (int i = 0; i < NKEYS; i++) {
        size_t kn = strlen(KEYS[i]);
        memset(g_clavis, 0, sizeof g_clavis);
        memcpy(g_clavis, KEYS[i], kn);
        uint64_t pos = exs_documentum_quaere(g_arbor, g_buf, is, n, g_clavis, kn);
        if (pos >= n) { r->k[i].state = 0; continue; }
        exs_indicium_lege(g_indicium, g_buf, is + 8 + pos * EXSEC_INDICIUM_BYTES);
        uint64_t v = exs_documentum_proba(g_buf, g_len, g_indicium);
        r->k[i].state  = v == 0 ? 1 : -(int)v;
        r->k[i].size   = exsec_le32(g_indicium + EXSEC_INDICIUM_MAGNITUDO);
        r->k[i].offset = exsec_le64(g_indicium + EXSEC_INDICIUM_SITUS);
    }
    r->verdict = 0;
    return 0;
}

int main(void)
{
    kernel_init();
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);

    // ---- the container, into the reader's fixed buffer ----
    FILE *f = fopen(DB_PATH, "rb");
    assertf(f, "exsec-streamdb-demo: %s not found", DB_PATH);
    g_len = fread(g_buf, 1, sizeof g_buf, f);
    int more = fgetc(f);
    fclose(f);
    assertf(more == EOF, "exsec-streamdb-demo: container exceeds %u bytes", EXSEC_CAPACITY);

    // ---- the Exsecutor reader, on a 32 KB stack ----
    // Priority +1, ABOVE main, so the thread runs to completion inside
    // kthread_new (libdragon kernel.c: a new thread at >= the current priority
    // is switched to immediately), and only then is it joined.
    //
    // THE ORDER IS FORCED BY A LIBDRAGON BUG, found by running this ROM. The
    // first version used priority -1 and joined a thread that had not run yet.
    // kthread_join's blocking path records `th->joiner = th_cur` and calls
    // KTHREAD_SWITCH() -- an explicit syscall -- WITHOUT putting the joining
    // thread on any list, and the scheduler asserts at kernel.c:307 that a
    // thread switching by syscall is already on one. So every join of a thread
    // that has not finished dies in the Inspector with
    //   ASSERTION FAILED: th_cur->flags & TH_FLAG_INLIST   (thread: main)
    // The path that works is the other one: a non-detached thread that exits
    // with no joiner is parked as WAITFORJOIN, not freed, and kthread_join then
    // frees it without switching. Running first is what puts it on that path.
    ExsecResult res = { .verdict = -1 };
    kthread_t *th = kthread_new("exsecutor", EXSEC_STACK, 1, lector, &res);
    assertf(th, "exsec-streamdb-demo: kthread_new failed");
    kthread_join(th);

    // ---- Kiln's own reader, on the same container ----
    int all_agree = res.verdict == 0;
    uint32_t kiln_docs = 0;
    size_t need = kiln_asset_probe_size(DB_PATH);
    assertf(need > 0, "exsec-streamdb-demo: kiln_asset_probe_size failed");
    void *arena = malloc(need);
    KilnAsset *db = kiln_asset_open(DB_PATH, arena, need);
    assertf(db, "exsec-streamdb-demo: kiln_asset_open failed");
    kiln_docs = kiln_asset_count(db);
    if (kiln_docs != res.docs) all_agree = 0;

    for (int i = 0; i < NKEYS; i++) {
        size_t kn = strlen(KEYS[i]);
        size_t ksize = kiln_asset_size(db, KEYS[i], kn);
        int agree;
        if (res.k[i].state == 0) {
            agree = ksize == 0;
        } else if (res.k[i].state < 0) {
            agree = 0;              // the Exsecutor reader rejected a record
        } else if (ksize != res.k[i].size) {
            agree = 0;
        } else {
            uint8_t *p = malloc(ksize ? ksize : 1);
            size_t got = ksize;
            int rc = kiln_asset_load(db, KEYS[i], kn, p, &got);
            agree = rc == STREAMDB_EMB_OK && got == ksize &&
                    res.k[i].offset + ksize <= g_len &&
                    memcmp(p, g_buf + res.k[i].offset, ksize) == 0;
            free(p);
        }
        res.k[i].agree = agree;
        if (!agree) all_agree = 0;
    }

    for (;;) {
        joypad_poll();
        kiln_frame_begin();
        kiln_gui_begin();

        color_t ink  = RGBA32(232, 232, 240, 255);
        color_t head = RGBA32(0, 245, 212, 255);
        color_t ok   = RGBA32(80, 230, 120, 255);
        color_t bad  = RGBA32(255, 90, 90, 255);

        kiln_gui_panel(8, 8, 304, 150, RGBA32(10, 10, 24, 220), head);
        kiln_gui_text(14, 22, head, "EXSECUTOR STREAMDB READER");
        kiln_gui_text(14, 36, ink, "on a %d-byte thread stack", EXSEC_STACK);
        kiln_gui_text(14, 48, res.verdict == 0 ? ok : bad,
                      "open verdict %d", res.verdict);
        kiln_gui_text(14, 60, ink, "docs %lu (kiln %lu)  nodes %lu",
                      (unsigned long)res.docs, (unsigned long)kiln_docs,
                      (unsigned long)res.nodes);
        for (int i = 0; i < NKEYS; i++) {
            const char *what = res.k[i].state > 0 ? "found" :
                               res.k[i].state == 0 ? "absent" : "BAD";
            kiln_gui_text(14, 76 + 12 * i, res.k[i].agree ? ok : bad,
                          "%-16s %-6s %5lu %s", KEYS[i], what,
                          (unsigned long)res.k[i].size,
                          res.k[i].agree ? "AGREE" : "DIFFER");
        }
        kiln_gui_text(14, 120, all_agree ? ok : bad,
                      all_agree ? "BOTH READERS AGREE" : "READERS DISAGREE");

        kiln_gui_end();
        kiln_frame_end();
    }
}
