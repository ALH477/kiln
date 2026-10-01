// SPDX-License-Identifier: MIT
//
// A dungeon grown from one 64-bit seed by code written in Exsecutor, running
// on the N64 -- and checked, on the console, against an independent oracle.
//
// WHAT THE SCREEN SHOWS. A 64x64 chunk, three pixels a tile, with its seed and
// where it sits in the world; on the right what the generator reported (walkable
// tiles, stream draws, roughened and pruned tiles, the CRC-32 of the 4,096
// bytes) and what THIS CONSOLE measured generating it: milliseconds, and the
// high-water mark of the thread's stack. The bottom line says whether four chunks
// re-derived at boot agree with prototypes/dungeon_oracle.py's.
//
// THE SEED ENTERS ONCE, AT THE BOUNDARY. exs_fig_dungeon_chunk is a pure
// function of its seed: it reads no clock and has no way to. The only place a
// clock is read in this file is the A button, which makes a new WORLD from the
// COP0 count and hands it in. Everything after that -- 1,000-odd stream draws,
// twelve rooms, five cellular passes -- is arithmetic, so the same world and
// index make the same 4,096 bytes on every console and on a PC.
//
// THE PETABYTE. A chunk is 4,096 bytes and 10^15 / 4,096 is exactly
// 244,140,625,000, so the first petabyte is chunk indices 0..244,140,624,999
// and Z jumps to the last one. Nothing is stored: a chunk is a function of
// (world, index), and semina_furore (Furor Petabytorum, the seed module) is a bijection of the index, so no two of
// them share a seed. The generator costs 2.5 million instructions a chunk
// (examples/dungeon/README.md in the Exsecutor repo), which is why this is a
// loading-screen technique and not a per-frame one.
//
// CONTROLS  D-pad left/right: chunk -1/+1    D-pad up/down: +64/-64
//           Z: the last chunk of the petabyte   A: a new world from the clock
//           START: back to the pinned world
//
// NOT VERIFIED AS OF WRITING: this ROM has not been built or booted. The
// parity check (nix/checks/exsec-dungeon-parity.nix) holds the generated unit
// and dungeon_view.c to the oracle natively; nothing has held main.c to a
// console. The numbers it displays are therefore the first measurements of
// their kind, not confirmations of any estimate.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>

#include <stdint.h>
#include <string.h>

#include "exsec_dungeon.h"
#include "dungeon_view.h"
#include "dungeon_golden.h"

#define GEN_STACK    32768   /* the StreamDB reader's, which is known to fit a kthread */
#define STACK_PAINT  0xA5
#define PB_CHUNKS    UINT64_C(244140625000)   /* 10^15 bytes / 4,096 */

static unsigned char g_work[EXSEC_DUNGEON_WORK_BYTES];
static DungeonRun    g_runs[DUNGEON_VIEW_MAX_RUNS];

// The generated unit's one import. A trap inside Exsecutor code -- an overflow,
// a failed bounds check -- lands here with its abort kind.
_Noreturn void exsrt_abortus(unsigned kind)
{
    assertf(0, "exsecutor: abortus %u", kind);
    for (;;) { }
}

typedef struct {
    int      ran;        // the thread body executed before kthread_new returned
    uint64_t seed;       // in
    uint64_t walkable;   // out
    uint64_t stats[3];   // draws, boundary tiles roughened, floor tiles pruned
    uint32_t crc;
    uint32_t ticks;      // COP0 ticks the generator took, on this console
} GenJob;

// Paint the unused part of this thread's stack, from the bottom up to a margin
// below this function's own frame, so a scan afterwards finds how deep it went.
// Addresses and not pointers, for the reason exsec-streamdb-demo's main.c gives
// at length: pointer arithmetic outside the object is undefined, and GCC used it.
__attribute__((noinline))
static void paint_stack(void)
{
    const uintptr_t base = (uintptr_t)kthread_current() - GEN_STACK;
    const uintptr_t stop = (uintptr_t)__builtin_frame_address(0) - 1024;
    if (stop > base) memset((void *)base, STACK_PAINT, stop - base);
}

static int generate_thread(void *arg)
{
    GenJob *j = arg;
    paint_stack();
    j->ran = 1;
    const uint32_t t0 = TICKS_READ();
    j->walkable = exs_fig_dungeon_chunk(g_work, (unsigned char *)j->stats, j->seed);
    j->ticks = (uint32_t)TICKS_DISTANCE(t0, TICKS_READ());
    j->crc = (uint32_t)exs_fig_dungeon_crc32(g_work);
    return 0;
}

// Runs one chunk on a GEN_STACK-byte thread and returns that thread's measured
// stack depth. Priority +1, ABOVE main, so the thread runs to completion inside
// kthread_new and is only then joined -- the same constraint, for the same
// libdragon bug, that exsec-streamdb-demo's main.c documents.
static uint32_t run_job(GenJob *job)
{
    memset(job->stats, 0, sizeof job->stats);
    job->ran = 0;
    kthread_t *th = kthread_new("dungeon", GEN_STACK, 1, generate_thread, job);
    assertf(th, "exsec-dungeon-demo: kthread_new failed");
    uint32_t peak = 0;
    if (job->ran) {
        const uintptr_t top = (uintptr_t)th;
        uintptr_t a = top - GEN_STACK;
        while (a < top && *(const unsigned char *)a == STACK_PAINT) a++;
        peak = (uint32_t)(top - a);
    }
    kthread_join(th);
    return peak;
}

// The four chunks the oracle wrote down, re-derived here. Returns how many
// agree in every field -- seed, walkable count, draws, roughened, pruned, CRC.
static int self_check(void)
{
    int ok = 0;
    for (int i = 0; i < DUNGEON_GOLDEN_COUNT; i++) {
        const DungeonGolden *g = &DUNGEON_GOLDEN[i];
        GenJob j;
        memset(&j, 0, sizeof j);
        j.seed = exs_semina_furore(g->world, g->index);
        run_job(&j);
        const int agree = j.seed == g->seed && j.walkable == g->walkable &&
                          j.stats[0] == g->draws && j.stats[1] == g->boundary &&
                          j.stats[2] == g->pruned && j.crc == g->crc;
        debugf("dungeon: golden %d %s  index %llu seed %016llx walk %llu crc %08lx\n",
               i, agree ? "AGREE" : "DISAGREE", (unsigned long long)g->index,
               (unsigned long long)j.seed, (unsigned long long)j.walkable,
               (unsigned long)j.crc);
        if (agree) ok++;
    }
    return ok;
}

static const color_t INK  = { 232, 232, 240, 255 };
static const color_t HEAD = { 0, 245, 212, 255 };
static const color_t GOOD = { 80, 230, 120, 255 };
static const color_t BAD  = { 255, 90, 90, 255 };
static const color_t DIM  = { 144, 152, 176, 255 };
static const color_t FILL = { 10, 10, 24, 255 };

#define MAP_X  6
#define MAP_Y  24
#define TILE   3

static color_t tile_color(uint8_t kind)
{
    switch (kind) {
    case EXSEC_TILE_FLOOR: return RGBA32(72, 88, 124, 255);
    case EXSEC_TILE_DOOR:  return RGBA32(255, 196, 64, 255);
    case EXSEC_TILE_TRAP:  return RGBA32(255, 70, 70, 255);
    default:               return FILL;
    }
}

int main(void)
{
    kernel_init();
    fig_engine_init(RESOLUTION_320x240);
    debug_init_isviewer();
    joypad_init();
    fig_input_init();

    // ---- the mixer against splitmix64's published vector, then the oracle ----
    const int mix_ok = exs_misce_furore(UINT64_C(0x9E3779B97F4A7C15)) ==
                       UINT64_C(0xE220A8397B1DCDAF);
    const int golden_ok = self_check();
    debugf("dungeon: mixer %s, golden %d/%d\n", mix_ok ? "AGREE" : "DISAGREE",
           golden_ok, DUNGEON_GOLDEN_COUNT);

    uint64_t world = DUNGEON_GOLDEN[0].world;
    uint64_t index = 0;
    int clock_world = 0;

    GenJob job;
    uint32_t stack_peak = 0;
    int nruns = 0;
    uint32_t census[4];
    int dirty = 1;

    for (;;) {
        fig_input_update();
        if (fig_input_pressed(0, FIG_BTN_DR) && index + 1 < PB_CHUNKS) { index += 1;  dirty = 1; }
        if (fig_input_pressed(0, FIG_BTN_DL) && index >= 1)            { index -= 1;  dirty = 1; }
        if (fig_input_pressed(0, FIG_BTN_DU)) {
            index = index + 64 < PB_CHUNKS ? index + 64 : PB_CHUNKS - 1;
            dirty = 1;
        }
        if (fig_input_pressed(0, FIG_BTN_DD)) { index = index >= 64 ? index - 64 : 0; dirty = 1; }
        if (fig_input_pressed(0, FIG_BTN_Z))  { index = PB_CHUNKS - 1; dirty = 1; }
        if (fig_input_pressed(0, FIG_BTN_A)) {
            // the one place a clock is read: a new world, handed in as data
            world = exs_misce_furore(((uint64_t)TICKS_READ() << 32) ^
                                        (uint64_t)TICKS_READ() ^ get_ticks_ms());
            index = 0; clock_world = 1; dirty = 1;
        }
        if (fig_input_pressed(0, FIG_BTN_START)) {
            world = DUNGEON_GOLDEN[0].world; index = 0; clock_world = 0; dirty = 1;
        }

        if (dirty) {
            memset(&job, 0, sizeof job);
            job.seed = exs_semina_furore(world, index);
            stack_peak = run_job(&job);
            nruns = dungeon_view_runs(g_work, g_runs);
            dungeon_view_census(g_work, census);
            dirty = 0;
        }

        surface_t *surf = display_get();
        rdpq_attach(surf, NULL);
        fig_gui_begin();
        fig_gui_rect(0, 0, 320, 240, FILL);

        fig_gui_panel(4, 4, 312, 16, FILL, HEAD);
        fig_gui_text(10, 16, HEAD, "EXSECUTOR DUNGEON");
        fig_gui_text(190, 16, DIM, "%s", clock_world ? "clock world" : "pinned world");

        fig_gui_panel(MAP_X - 2, MAP_Y - 2, 64 * TILE + 4, 64 * TILE + 4, FILL, HEAD);
        for (int i = 0; i < nruns; i++) {
            fig_gui_rect(MAP_X + g_runs[i].x * TILE, MAP_Y + g_runs[i].y * TILE,
                         g_runs[i].w * TILE, TILE, tile_color(g_runs[i].kind));
        }

        const int x0 = 206;
        const unsigned long us = (unsigned long)TICKS_TO_US(job.ticks);
        fig_gui_text(x0, 36, DIM, "WORLD");
        fig_gui_text(x0, 47, INK, "%016llX", (unsigned long long)world);
        fig_gui_text(x0, 63, DIM, "CHUNK");
        if (index >= UINT64_C(1000000000))
            fig_gui_text(x0, 74, INK, "%lu%09lu", (unsigned long)(index / UINT64_C(1000000000)),
                         (unsigned long)(index % UINT64_C(1000000000)));
        else
            fig_gui_text(x0, 74, INK, "%lu", (unsigned long)index);
        fig_gui_text(x0, 85, DIM, "of 244140625000");
        fig_gui_text(x0, 101, DIM, "SEED");
        fig_gui_text(x0, 112, INK, "%016llX", (unsigned long long)job.seed);
        fig_gui_text(x0, 128, INK, "walk %lu", (unsigned long)job.walkable);
        fig_gui_text(x0, 139, INK, "door %lu trap %lu", (unsigned long)census[EXSEC_TILE_DOOR],
                     (unsigned long)census[EXSEC_TILE_TRAP]);
        fig_gui_text(x0, 150, INK, "draws %lu", (unsigned long)job.stats[0]);
        fig_gui_text(x0, 161, INK, "edge %lu", (unsigned long)job.stats[1]);
        fig_gui_text(x0, 172, job.stats[2] ? HEAD : INK, "pruned %lu", (unsigned long)job.stats[2]);
        fig_gui_text(x0, 183, INK, "crc %08lX", (unsigned long)job.crc);
        fig_gui_text(x0, 199, HEAD, "gen %lu.%02lu ms", us / 1000, (us % 1000) / 10);
        fig_gui_text(x0, 210, stack_peak > GEN_STACK - 2048 ? BAD : DIM,
                     "stack %lu/%d", (unsigned long)stack_peak, GEN_STACK);

        const int all_ok = mix_ok && golden_ok == DUNGEON_GOLDEN_COUNT;
        fig_gui_text(MAP_X, 232, all_ok ? GOOD : BAD,
                     all_ok ? "ORACLE AGREE %d/%d + MIXER" : "ORACLE DISAGREE %d/%d",
                     golden_ok, DUNGEON_GOLDEN_COUNT);
        fig_gui_text(206, 232, DIM, "Z last  A new");

        fig_gui_end();
        rdpq_detach_show();
    }
}
