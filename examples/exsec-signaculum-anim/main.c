// SPDX-License-Identifier: MIT
//
// exsec-signaculum-anim -- the Exsecutor logo's 28-frame rock, every frame
// rasterized ON THE VR4300 by code the Exsecutor compiler emitted, then
// played back from RDRAM.
//
// WHY A CACHE AND NOT A LIVE RENDER. exs_signaculum_pingue clears and fills a
// 196,608-byte framebuffer and a 524,288-byte f64 1/z z-buffer per frame --
// 720,896 bytes the clear alone must stream through an 8 KB write-back
// D-cache before a single one of the 2,981 faces is rasterized. On x86-64 the
// same pinned unit takes 1.23 ms a frame; the VR4300 is 93.75 MHz, in-order,
// and its f64 divide is not pipelined. Live playback would be a slideshow, so
// this demo bakes all 28 frames once at boot -- with the real per-frame cost
// on screen, measured here rather than estimated -- and then loops the cache.
// The bake IS the demonstration; the playback is what the bake buys.
//
// TWO RAM MODES, chosen by the console and not by a button. ./dev drive does
// not work on this machine (flake.nix's uinput note), so nothing here waits
// on input:
//
//   8 MB (Expansion Pak)  28 frames at the full 256x256, RGBA5551
//                         -> 131,072 B/frame, 3,670,016 B of cache
//   4 MB (stock console)  28 frames at 128x128, 2x2 box-averaged, RGBA5551
//                         -> 32,768 B/frame, 917,504 B of cache, pixel-
//                            doubled on present
//
// The 8 MB plan is ATTEMPTED, not assumed: the engine's own 706 KB of soft3d
// statics, libdragon and the display buffers come out of the same pool, so
// the full cache is malloc'd and the half-resolution plan is the fallback if
// that fails. Whichever ran is on screen and on ISViewer -- a demo that
// silently degraded would be telling the same kind of lie the CRCs exist to
// catch.
//
// THE MOTION is logo/README.md's GIF row, reproduced exactly:
// yaw = 0.45*sin(2*pi*t), pitch = -0.14 + 0.05*cos(2*pi*t) over 28 frames. A
// ROCK, not a turntable -- the mark is an extruded flat form and a full turn
// would take it edge-on. Not a palindrome either: frame 28-i mirrors the yaw
// but keeps the pitch, and the mark is asymmetric, so the table is not half a
// table played backwards.
//
// WHAT IS CERTIFIED. Each frame's framebuffer CRC-32 is compared against
// crc_pins.h, measured on the host tier from the same pinned
// signaculum_x86_64.gen.c. The hero pin is exsec-signaculum-demo's own
// EXPECTED_CRC, unchanged, so this extends that certificate across 29 views
// instead of replacing it. The animation is driven by patching 36 bytes --
// the rotation matrix at EXSG offset 20 -- so the mesh, the camera and the
// baked colours are bit-identical in every frame by construction.
#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_soft3d.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "crc_pins.h"

#define EXSG_PATH  "rom:/signaculum.exsg"
#define ROT_PATH   "rom:/rotations.bin"
#define ROT_BYTES  (SIG_ANIM_FRAMES * FIG_SOFT3D_ROT_BYTES)

// 36,176 bytes of callee frame (gen/PROVENANCE.md) plus this file's own; the
// sibling demo's figure, for the same core.
#define RENDER_STACK 49152
#define STACK_PAINT  0xA5

#define FULL_SIDE 256u
#define HALF_SIDE 128u

static uint8_t  g_exsg[FIG_SOFT3D_EXSG_BYTES];
static uint8_t  g_rot[ROT_BYTES];
static uint16_t *g_cache;      // SIG_ANIM_FRAMES * side * side RGBA5551
static uint32_t  g_side;       // FULL_SIDE or HALF_SIDE
static uint32_t  g_cache_bytes;

/* The ONE symbol the generated unit imports (kiln_soft3d.h): a trap in
 * Exsecutor-emitted code -- an overflowing add or a bad decode -- lands here.
 * Same body as exsec-signaculum-demo's, deliberately: a trap during the bake
 * is a defect to see in the Inspector, not a frame to skip. */
_Noreturn void exsrt_abortus(unsigned kind)
{
    assertf(0, "exsecutor: abortus %u", kind);
    for (;;) { }
}

/* CRC-32/ISO-HDLC, table-free: 28 runs over 192 KB at boot, not a hot path.
 * Identical to exsec-signaculum-demo's, deliberately -- the pins are shared. */
static uint32_t crc32_iso_hldlc(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1) + 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* RGB888 -> RGBA5551 by top bits: the shift fig_soft3d_present already uses.
 * No dither, no rounding -- a frame's cache entry is a deterministic function
 * of its framebuffer. */
static inline uint16_t pack5551(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1u);
}

__attribute__((noinline))
static void paint_stack(void)
{
    const uintptr_t base = (uintptr_t)kthread_current() - RENDER_STACK;
    const uintptr_t stop = (uintptr_t)__builtin_frame_address(0) - 1024;
    if (stop > base) memset((void *)base, STACK_PAINT, stop - base);
}

typedef struct { int ran; uint8_t status; uint32_t crc; } RenderResult;

static int render(void *arg)
{
    RenderResult *r = arg;
    paint_stack();
    r->ran = 1;
    r->status = fig_soft3d_render_frame();
    r->crc = (r->status == FIG_SOFT3D_OK)
           ? crc32_iso_hldlc(fig_soft3d_framebuffer(), FIG_SOFT3D_FB_BYTES)
           : 0u;
    return 0;
}

/* Fold the 256x256 RGB888 frame into one cache slot. At FULL_SIDE this is a
 * pack per pixel; at HALF_SIDE it is a 2x2 box average first -- integer sums
 * shifted by two, so there is no rounding mode to disagree about. */
static void stash(uint32_t frame)
{
    const uint8_t *src = fig_soft3d_framebuffer();
    uint16_t *dst = g_cache + (size_t)frame * g_side * g_side;

    if (g_side == FULL_SIDE) {
        for (uint32_t i = 0; i < FULL_SIDE * FULL_SIDE; i++)
            dst[i] = pack5551(src[i * 3], src[i * 3 + 1], src[i * 3 + 2]);
        return;
    }
    for (uint32_t y = 0; y < HALF_SIDE; y++) {
        for (uint32_t x = 0; x < HALF_SIDE; x++) {
            const uint32_t o0 = ((y * 2) * FULL_SIDE + x * 2) * 3;
            const uint32_t o1 = o0 + 3;
            const uint32_t o2 = o0 + FULL_SIDE * 3;
            const uint32_t o3 = o2 + 3;
            const uint32_t r = (uint32_t)src[o0]     + src[o1]     + src[o2]     + src[o3];
            const uint32_t g = (uint32_t)src[o0 + 1] + src[o1 + 1] + src[o2 + 1] + src[o3 + 1];
            const uint32_t b = (uint32_t)src[o0 + 2] + src[o1 + 2] + src[o2 + 2] + src[o3 + 2];
            dst[y * HALF_SIDE + x] = pack5551((uint8_t)(r >> 2),
                                              (uint8_t)(g >> 2),
                                              (uint8_t)(b >> 2));
        }
    }
}

/* One cache slot into a 320x240 surface: the source square is centred, 32 px
 * of letterbox each side, and 8 of its 256 rows cropped (256 - 240) -- the
 * mapping fig_soft3d_present documents. At HALF_SIDE each source pixel covers
 * 2x2, so the logo occupies the same screen area in both modes. */
static void present_cached(surface_t *disp, uint32_t frame)
{
    const uint16_t *src = g_cache + (size_t)frame * g_side * g_side;
    const uint32_t shift = (g_side == FULL_SIDE) ? 0u : 1u;
    for (uint32_t y = 0; y < 240; y++) {
        uint16_t *row = (uint16_t *)((uint8_t *)disp->buffer + (size_t)y * disp->stride);
        const uint32_t sy = (y + 8u) >> shift;
        for (uint32_t x = 0; x < 320; x++) {
            if (x < 32 || x >= 288) { row[x] = 1u; continue; }  /* opaque black */
            row[x] = src[sy * g_side + ((x - 32u) >> shift)];
        }
    }
    data_cache_hit_writeback(disp->buffer, (size_t)disp->stride * 240);
}

static uint32_t load(const char *path, uint8_t *into, uint32_t want)
{
    FILE *f = fopen(path, "rb");
    assertf(f, "exsec-signaculum-anim: %s not found", path);
    uint32_t n = (uint32_t)fread(into, 1, want, f);
    int more = fgetc(f);
    fclose(f);
    assertf(more == EOF && n == want,
            "exsec-signaculum-anim: %s is not %lu bytes", path, (unsigned long)want);
    return n;
}

int main(void)
{
    kernel_init();
    fig_engine_init(RESOLUTION_320x240);
    debug_init_isviewer();
    dfs_init(DFS_DEFAULT_LOCATION);

    load(EXSG_PATH, g_exsg, FIG_SOFT3D_EXSG_BYTES);
    load(ROT_PATH,  g_rot,  ROT_BYTES);
    fig_soft3d_init(g_exsg, FIG_SOFT3D_EXSG_BYTES);

    // ---- pick a cache plan: ask for the full one, accept the fallback ----
    const uint32_t ram = (uint32_t)get_memory_size();
    const uint32_t full_bytes = SIG_ANIM_FRAMES * FULL_SIDE * FULL_SIDE * 2u;
    const uint32_t half_bytes = SIG_ANIM_FRAMES * HALF_SIDE * HALF_SIDE * 2u;

    g_cache = NULL;
    if (ram > 4u * 1024u * 1024u) {
        g_cache = malloc(full_bytes);
        if (g_cache) { g_side = FULL_SIDE; g_cache_bytes = full_bytes; }
    }
    if (!g_cache) {
        g_cache = malloc(half_bytes);
        assertf(g_cache, "exsec-signaculum-anim: no room for even the %lu-byte cache",
                (unsigned long)half_bytes);
        g_side = HALF_SIDE; g_cache_bytes = half_bytes;
    }
    debugf("signaculum-anim: ram=%lu KiB mode=%lux%lu cache=%lu B\n",
           (unsigned long)(ram / 1024), (unsigned long)g_side,
           (unsigned long)g_side, (unsigned long)g_cache_bytes);

    // ---- the bake: 28 renders, each on its own 48 KiB thread ----
    // Priority +1 so the thread completes inside kthread_new before the join,
    // the constraint exsec-streamdb-demo and exsec-signaculum-demo both
    // record (kthread_join of a still-running thread asserts).
    uint32_t agree = 0, worst_us = 0, total_us = 0, stack_peak = 0;
    uint8_t  first_bad_status = FIG_SOFT3D_OK;
    uint32_t first_bad_frame = SIG_ANIM_FRAMES;

    for (uint32_t i = 0; i < SIG_ANIM_FRAMES; i++) {
        fig_soft3d_set_rotation(g_rot + (size_t)i * FIG_SOFT3D_ROT_BYTES,
                                FIG_SOFT3D_ROT_BYTES);
        RenderResult res = { .status = 255 };
        const uint32_t t0 = TICKS_READ();
        kthread_t *th = kthread_new("sigbake", RENDER_STACK, 1, render, &res);
        assertf(th, "exsec-signaculum-anim: kthread_new failed at frame %lu",
                (unsigned long)i);
        const uint32_t us = (uint32_t)TICKS_TO_US(TICKS_READ() - t0);

        if (res.ran && stack_peak == 0) {
            const uintptr_t top = (uintptr_t)th;
            uintptr_t a = top - RENDER_STACK;
            while (a < top && *(const unsigned char *)a == STACK_PAINT) a++;
            stack_peak = (uint32_t)(top - a);
        }
        kthread_join(th);

        total_us += us;
        if (us > worst_us) worst_us = us;

        if (res.status == FIG_SOFT3D_OK && res.crc == sig_frame_crc[i]) {
            agree++;
        } else if (first_bad_frame == SIG_ANIM_FRAMES) {
            first_bad_frame = i;
            first_bad_status = res.status;
            debugf("signaculum-anim: frame %lu DISAGREE status=%u crc=0x%08lx want=0x%08lx\n",
                   (unsigned long)i, res.status, (unsigned long)res.crc,
                   (unsigned long)sig_frame_crc[i]);
        }
        stash(i);

        // A progress bar, so a ~7-second bake does not look like a hang.
        surface_t *d = display_get();
        graphics_fill_screen(d, 0);
        graphics_set_color(graphics_make_color(0, 245, 212, 255), 0);
        graphics_draw_text(d, 84, 104, "RASTERIZING ON THE VR4300");
        graphics_set_color(graphics_make_color(232, 232, 240, 255), 0);
        char line[64];
        snprintf(line, sizeof line, "frame %2lu / %u   %lu.%01lu ms",
                 (unsigned long)(i + 1), SIG_ANIM_FRAMES,
                 (unsigned long)(us / 1000), (unsigned long)((us % 1000) / 100));
        graphics_draw_text(d, 104, 124, line);
        const uint32_t w = (uint32_t)(224u * (i + 1u) / SIG_ANIM_FRAMES);
        for (uint32_t yy = 140; yy < 148; yy++) {
            uint16_t *row = (uint16_t *)((uint8_t *)d->buffer + (size_t)yy * d->stride);
            for (uint32_t xx = 48; xx < 48 + w; xx++) row[xx] = pack5551(0, 245, 212);
        }
        data_cache_hit_writeback(d->buffer, (size_t)d->stride * 240);
        display_show(d);
    }

    const uint32_t mean_us = total_us / SIG_ANIM_FRAMES;
    const int all_agree = (agree == SIG_ANIM_FRAMES);
    debugf("signaculum-anim: %s %lu/%u frames  mean=%lu us worst=%lu us "
           "stack=%lu/%u mode=%lux%lu\n",
           all_agree ? "AGREE" : "DISAGREE", (unsigned long)agree,
           SIG_ANIM_FRAMES, (unsigned long)mean_us, (unsigned long)worst_us,
           (unsigned long)stack_peak, RENDER_STACK,
           (unsigned long)g_side, (unsigned long)g_side);
    if (!all_agree)
        debugf("signaculum-anim: first bad frame %lu status=%u\n",
               (unsigned long)first_bad_frame, first_bad_status);

    // ---- playback: the cache, on a loop, with the numbers underneath ----
    // Three display frames per animation frame: 28 frames at ~20 fps is one
    // rock every 1.4 s, which is the GIF's own cadence.
    const color_t INK  = { 232, 232, 240, 255 };
    const color_t HEAD = { 0, 245, 212, 255 };
    const color_t GOOD = { 80, 230, 120, 255 };
    const color_t BAD  = { 255, 90, 90, 255 };
    uint32_t frame = 0, hold = 0;
    for (;;) {
        surface_t *d = display_get();
        present_cached(d, frame);

        // THE SCREEN IS FORTY CHARACTERS WIDE. graphics_draw_text is an 8x8
        // font and the surface is 320 px, so a string starting at x=8 has 39
        // columns before it runs off the right edge -- and nothing clips it,
        // it simply overprints or vanishes. The first capture of this demo
        // had the title and the verdict overprinting ("28-FRAMERROCK8/2") and
        // the bottom line truncated mid-word, neither of which the CRCs can
        // see. Every string below is budgeted:
        //
        //   title   26 ch at x=8   ->  8..216
        //   verdict 11 ch at x=228 -> 228..316
        //   l1/l2   <= 38 ch at x=8
        graphics_set_color(graphics_make_color(HEAD.r, HEAD.g, HEAD.b, 255), 0);
        graphics_draw_text(d, 8, 4, "SIGNACULUM / 28-FRAME ROCK");
        char l1[48], l2[48], l3[24];
        snprintf(l1, sizeof l1, "%lux%lu cache %luKiB  %luMiB RAM",
                 (unsigned long)g_side, (unsigned long)g_side,
                 (unsigned long)(g_cache_bytes / 1024),
                 (unsigned long)(ram / (1024 * 1024)));
        snprintf(l2, sizeof l2, "bake %lu.%01lums  worst %lu.%01lums  f%02lu",
                 (unsigned long)(mean_us / 1000), (unsigned long)((mean_us % 1000) / 100),
                 (unsigned long)(worst_us / 1000), (unsigned long)((worst_us % 1000) / 100),
                 (unsigned long)frame);
        graphics_set_color(graphics_make_color(INK.r, INK.g, INK.b, 255), 0);
        graphics_draw_text(d, 8, 216, l1);
        graphics_draw_text(d, 8, 226, l2);
        const color_t v = all_agree ? GOOD : BAD;
        graphics_set_color(graphics_make_color(v.r, v.g, v.b, 255), 0);
        snprintf(l3, sizeof l3, "%s %lu/%u", all_agree ? "AGREE" : "BAD",
                 (unsigned long)agree, SIG_ANIM_FRAMES);
        graphics_draw_text(d, 228, 4, l3);

        data_cache_hit_writeback(d->buffer, (size_t)d->stride * 240);
        display_show(d);

        if (++hold >= 3) { hold = 0; frame = (frame + 1u) % SIG_ANIM_FRAMES; }
    }
}
