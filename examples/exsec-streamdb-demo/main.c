// SPDX-License-Identifier: MIT
//
// A StreamDB v3 reader written in Exsecutor, running on the N64, checked
// against Kiln's own reader on the same container.
//
// WHAT THIS ROM SHOWS, revealed one step at a time and then looped:
//
//   1. The container opens through the Exsecutor reader: header slot chosen,
//      both CRCs verified, index well-formed, trie flattened -- the same
//      sequence examples/streamdb/probatio.exsc runs in the Exsecutor repo,
//      where it is certified byte-for-byte against the upstream C reader.
//   2. It all runs on a kthread whose stack is 32,768 bytes. The reader's
//      traversal frame is 20,680 bytes (ADR 0016); before that work it was
//      127,184, which would not have fit this thread -- nor libdragon's 64 KB
//      main stack. The bar is the thread's MEASURED high-water mark: the stack
//      is painted before the reader runs and scanned after, and the tick in the
//      bar is PROVENANCE.md's static frame size.
//   3. Each key is looked up by BOTH readers, and a row flashes AGREE only if
//      they find the same documents with the same sizes and the same bytes.
//      One key is deliberately absent, so "not found" is compared too. Behind
//      the panel, one slab per key takes the row's colour; the absent key has
//      no slab, only its outline.
//   4. data/ave.txt, typed out from the bytes the Exsecutor reader located.
//
// Jump ROM: .#exsec-streamdb-demo-done holds the end of the reveal.
//
// The container is flake.nix's `exsecStreamdb`, packed by the upstream C writer
// like every other .streamdb in this repo.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_asset.h>
#include <kiln/kiln_prim.h>
#include <kiln/kiln_debugdraw.h>

#include <malloc.h>
#include <string.h>

#include "exsec_streamdb.h"

enum { JUMP_NONE, JUMP_DONE };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W     320
#define SCREEN_H     240
#define DB_PATH      "rom:/exsec.streamdb"
#define EXSEC_STACK  32768
#define STATIC_FRAME 20680   /* PROVENANCE.md: exs_arbor_percurre, -fstack-usage */
#define STACK_PAINT  0xA5
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
    int      ran;       // the thread body executed before kthread_new returned
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

// Paint the unused part of this thread's stack, from the bottom up to a margin
// below this function's own frame. libdragon puts the kthread_t at the TOP of
// the stack allocation (kernel.c: `th = thmem + STACK_GUARD + stack_size`), so
// the stack is exactly [kthread_current() - stack_size, kthread_current()).
//
// ADDRESSES, NOT POINTERS. The first version walked `p < &marker - 1024` with
// pointer arithmetic, which is undefined outside the object it starts from, and
// GCC used that: the loop compiled to an unconditional branch with no exit test
// and painted straight through the stack and on into the heap, so the ROM hung
// before its first frame. As uintptr_t the bound is ordinary arithmetic.
//
// The margin covers memset's own frame, if it has one. An interrupt taken
// meanwhile writes its 576-byte register dump below sp, which only ever makes
// a byte read as used, never the reverse.
__attribute__((noinline))
static void paint_stack(void)
{
    const uintptr_t base = (uintptr_t)kthread_current() - EXSEC_STACK;
    const uintptr_t stop = (uintptr_t)__builtin_frame_address(0) - 1024;
    if (stop > base) memset((void *)base, STACK_PAINT, stop - base);
}

// Runs on the 32 KB thread. Everything it touches is static, so the only stack
// it uses is the reader's own frames.
static int lector(void *arg)
{
    ExsecResult *r = arg;
    paint_stack();
    r->ran = 1;

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

// ── the reveal ─────────────────────────────────────────────────────────
#define T_OPEN     30
#define T_DOCS     60
#define T_ROW0     100
#define T_ROW_GAP  50
#define T_VERDICT  (T_ROW0 + NKEYS * T_ROW_GAP + 10)
#define T_TYPE     (T_VERDICT + 50)
#define TYPE_RATE  2            /* frames per character */
#define T_HOLD     300
#define FLASH      24

#define WRAP_COLS  49
#define WRAP_LINES 3

static const color_t INK  = { 232, 232, 240, 255 };
static const color_t HEAD = { 0, 245, 212, 255 };
static const color_t OK   = { 80, 230, 120, 255 };
static const color_t BAD  = { 255, 90, 90, 255 };
static const color_t DIM  = { 144, 152, 176, 255 };
static const color_t FILL = { 10, 10, 24, 255 };

/* A row's backing strip: a few frames alternating the verdict colour, then a
 * dark tint of it. Returns the ink the row's text should use on top. */
static color_t flash_strip(int x, int y, int w, int age, int good)
{
    const color_t hot = good ? OK : BAD;
    const color_t tint = good ? RGBA32(18, 56, 30, 255) : RGBA32(64, 18, 22, 255);
    if (age < FLASH && (age / 4) % 2 == 0) {
        kiln_gui_rect(x, y, w, 12, hot);
        return FILL;
    }
    kiln_gui_rect(x, y, w, 12, tint);
    return hot;
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

    // The finished thread's memory is still allocated until the join, so its
    // stack can be read here: the first byte above the bottom that is no longer
    // paint is the deepest the thread ever reached.
    uint32_t stack_peak = 0;
    if (res.ran) {
        const uintptr_t top = (uintptr_t)th;
        uintptr_t a = top - EXSEC_STACK;
        while (a < top && *(const unsigned char *)a == STACK_PAINT) a++;
        stack_peak = (uint32_t)(top - a);
    }
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

    // ---- the document, from the Exsecutor reader's own offset ----
    static char ave[256];
    int ave_len = 0;
    if (res.k[1].state > 0 && res.k[1].offset + res.k[1].size <= g_len) {
        ave_len = res.k[1].size < sizeof ave - 1 ? (int)res.k[1].size : (int)sizeof ave - 1;
        memcpy(ave, g_buf + res.k[1].offset, (size_t)ave_len);
        while (ave_len > 0 && (ave[ave_len - 1] == '\n' || ave[ave_len - 1] == '\r')) ave_len--;
    } else {
        ave_len = snprintf(ave, sizeof ave, "(the reader did not find data/ave.txt)");
    }
    ave[ave_len] = 0;

    // Word-wrap once: each line is [start, start+len) of `ave`.
    int line_start[WRAP_LINES], line_len[WRAP_LINES], nlines = 0;
    for (int at = 0; at < ave_len && nlines < WRAP_LINES; ) {
        int len = ave_len - at;
        if (len > WRAP_COLS) {
            len = WRAP_COLS;
            while (len > 0 && ave[at + len] != ' ') len--;
            if (len == 0) len = WRAP_COLS;
        }
        line_start[nlines] = at;
        line_len[nlines++] = len;
        at += len;
        while (at < ave_len && ave[at] == ' ') at++;
    }
    const int type_frames = ave_len * TYPE_RATE;
    const int t_loop = T_TYPE + type_frames + T_HOLD;

    // ---- the stage behind the panels ----
    KilnScene scene;
    kiln_scene_init(&scene);
    kiln_prim_stage(&scene, RGBA32(0x3C, 0x46, 0x64, 0xFF), 90.0f, 190.0f);
    scene.fov_deg = 60.0f;
    scene.near_z = 6.0f;
    scene.far_z = 200.0f;
    scene.cam_pos = (fm_vec3_t){{ 0, 34, -95 }};
    scene.cam_target = (fm_vec3_t){{ 0, 25, 0 }};

    KilnPrim floor_prim, slab_idle, slab_ok, slab_bad;
    kiln_prim_floor(&floor_prim, 120.0f, 12,
                    kiln_prim_rgba(0x6C, 0x76, 0x8E), kiln_prim_rgba(0x5A, 0x64, 0x7C));
    const fm_vec3_t slab_half = {{ 8, 10, 2 }};
    const fm_vec3_t slab_off = {{ 0, 10, 0 }};
    kiln_prim_box(&slab_idle, slab_off, slab_half, kiln_prim_rgba(0xB0, 0xB8, 0xCC),
                  kiln_prim_rgba(0x80, 0x88, 0xA0), kiln_prim_rgba(0x50, 0x58, 0x70));
    kiln_prim_box(&slab_ok, slab_off, slab_half, kiln_prim_rgba(0x90, 0xFF, 0xB0),
                  kiln_prim_rgba(0x40, 0xD0, 0x70), kiln_prim_rgba(0x20, 0x70, 0x38));
    kiln_prim_box(&slab_bad, slab_off, slab_half, kiln_prim_rgba(0xFF, 0x90, 0x90),
                  kiln_prim_rgba(0xE0, 0x40, 0x40), kiln_prim_rgba(0x70, 0x20, 0x20));

    KilnTransform floor_xf, slab_xf[NKEYS];
    kiln_transform_init(&floor_xf);
    for (int i = 0; i < NKEYS; i++) {
        kiln_transform_init(&slab_xf[i]);
        /* Camera looks down +Z, so screen-right is -X: key 0 on the left. */
        slab_xf[i].pos = (fm_vec3_t){{ 30.0f - 30.0f * (float)i, 0, 0 }};
        slab_xf[i].rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
    }

    int t = KILN_JUMP == JUMP_DONE ? T_TYPE + type_frames + 60 : 0;
    uint32_t pass = 1;
    float spin = 0.0f;

    for (;;) {
        joypad_poll();
        const float dt = 1.0f / 60.0f;
        spin += 0.5f * dt;

        const int row_t[NKEYS] = { T_ROW0, T_ROW0 + T_ROW_GAP, T_ROW0 + 2 * T_ROW_GAP };

        kiln_scene_update(&scene);
        kiln_frame_begin();
        kiln_scene_begin(&scene);

        kiln_transform_push(&floor_xf); kiln_prim_draw(&floor_prim); kiln_transform_pop();
        for (int i = 0; i < NKEYS; i++) {
            if (res.k[i].state == 0) continue;      // absent: an outline, below
            const int age = t - row_t[i];
            const KilnPrim *slab = &slab_idle;
            if (age >= 0 && !(age < FLASH && (age / 4) % 2 == 1))
                slab = res.k[i].agree ? &slab_ok : &slab_bad;
            slab_xf[i].rot_angle = spin + (float)i * 0.9f;
            kiln_transform_push(&slab_xf[i]);
            kiln_prim_draw(slab);
            kiln_transform_pop();
        }

        kiln_gui_begin();

        kiln_dd_begin(&scene, SCREEN_W, SCREEN_H);
        for (int i = 0; i < NKEYS; i++) {
            if (res.k[i].state != 0) continue;
            const int age = t - row_t[i];
            fm_vec3_t c = slab_xf[i].pos;
            c.v[1] += 10.0f;
            kiln_dd_box(c, slab_half, age < 0 ? DIM : res.k[i].agree ? OK : BAD);
        }
        kiln_dd_end();

        // ── A: the reader and its thread ──
        kiln_gui_panel(4, 4, 312, 50, FILL, HEAD);
        kiln_gui_text(10, 17, HEAD, "EXSECUTOR STREAMDB READER");
        if (KILN_JUMP == JUMP_NONE) kiln_gui_text(262, 17, DIM, "pass %lu", (unsigned long)pass);

        const float grow = t < T_OPEN ? (float)t / (float)T_OPEN : 1.0f;
        const float frac = (float)stack_peak / (float)EXSEC_STACK;
        kiln_gui_text(10, 30, INK, "stack");
        kiln_gui_bar(46, 24, 180, 7, frac * grow,
                     stack_peak > EXSEC_STACK - 1024 ? BAD : HEAD, RGBA32(42, 42, 62, 255));
        kiln_gui_rect(46 + (int)(180.0f * STATIC_FRAME / EXSEC_STACK), 22, 2, 11,
                      RGBA32(255, 216, 96, 255));
        kiln_gui_text(232, 30, INK, "%5lu/%d", (unsigned long)(stack_peak * grow), EXSEC_STACK);

        if (t >= T_OPEN)
            kiln_gui_text(10, 44, res.verdict == 0 ? OK : BAD, "open verdict %d", res.verdict);
        if (t >= T_DOCS)
            kiln_gui_text(118, 44, kiln_docs == res.docs ? OK : BAD,
                          "docs %lu=%lu  nodes %lu", (unsigned long)res.docs,
                          (unsigned long)kiln_docs, (unsigned long)res.nodes);

        // ── B: key by key ──
        kiln_gui_panel(4, 58, 312, 60, FILL, HEAD);
        for (int i = 0; i < NKEYS; i++) {
            const int age = t - row_t[i];
            const int y = 71 + 12 * i;
            if (age < 0) {
                kiln_gui_text(10, y, DIM, "%-16s ...", KEYS[i]);
                continue;
            }
            color_t ink = flash_strip(7, y - 10, 306, age, res.k[i].agree);
            const char *what = res.k[i].state > 0 ? "found" :
                               res.k[i].state == 0 ? "absent" : "BAD";
            kiln_gui_text(10, y, ink, "%-16s %-6s %5lu  %s", KEYS[i], what,
                          (unsigned long)res.k[i].size,
                          res.k[i].agree ? "AGREE" : "DIFFER");
        }
        if (t >= T_VERDICT) {
            color_t ink = flash_strip(7, 99, 306, t - T_VERDICT, all_agree);
            kiln_gui_text(10, 109, ink, all_agree ? "BOTH READERS AGREE" : "READERS DISAGREE");
        }

        // ── C: the document ──
        kiln_gui_panel(4, 178, 312, 58, FILL, HEAD);
        kiln_gui_text(10, 191, DIM, "data/ave.txt, as the Exsecutor reader found it");
        if (t >= T_TYPE) {
            const int typed = (t - T_TYPE) / TYPE_RATE;
            for (int l = 0; l < nlines; l++) {
                int n = typed - line_start[l];
                if (n <= 0) break;
                if (n > line_len[l]) n = line_len[l];
                kiln_gui_text(10, 204 + 12 * l, INK, "%.*s", n, ave + line_start[l]);
                const int done = typed >= ave_len;
                if ((n < line_len[l] || l == nlines - 1) && (!done || (t / 20) % 2 == 0))
                    kiln_gui_rect(11 + 6 * n, 196 + 12 * l, 5, 9, HEAD);
                if (n < line_len[l]) break;
            }
        }

        kiln_gui_end();
        kiln_frame_end();

        if (KILN_JUMP == JUMP_NONE && ++t >= t_loop) { t = 0; pass++; }
    }
}
