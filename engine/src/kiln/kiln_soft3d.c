/* SPDX-License-Identifier: MIT
 *
 * kiln_soft3d.c — storage and presentation around the Exsecutor signaculum
 * core. See kiln_soft3d.h for the contract and gen/PROVENANCE.md for what
 * the core is and what it is certified against.
 */

#include "kiln_soft3d.h"

#include <string.h>

/* The generated unit, included into THIS translation unit — on purpose, the
 * way examples/exsec-streamdb-demo's exsec_proto_check.c pairs its
 * hand-written header with lector_streamdb.gen.c: kiln_soft3d.h's
 * prototypes above and the definitions below must be one compile, so a
 * drift between them is "conflicting types", never silent cross-object UB.
 *
 * `#if defined(N64)`: n64.mk defines N64 for the console build and the host
 * tier never does (kiln_soft3d.h explains why this is the switch). The
 * mips64 unit asserts 32-bit addresses and simply cannot feed a host
 * compiler; the x86_64 unit asserts the IEEE subset it runs, which the host
 * tier holds anyway (nix/host.nix's -ffp-contract=off).
 *
 * -Wno-unused-function rides as a pragma here rather than a flag so the
 * HOST tier — which compiles this file with a -Werror nobody may edit
 * host.nix to relax — accepts the unit's fixed helper blob: a unit that
 * uses no signed-compare helper still carries it, and its emission is
 * exsc's fixed prologue, not a bug to silence one helper at a time. The
 * N64 side gets the same treatment as an explicit flag in engine/Makefile,
 * because that Makefile's comment is where n64.mk's -ffast-math ordering is
 * spelled out. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#if defined(N64)
#include "gen/signaculum_mips64.gen.c"
#else
#include "gen/signaculum_x86_64.gen.c"
#endif
#pragma GCC diagnostic pop

/* The borrow: all three buffers are the core's caller-owned storage, static
 * .bss here — never malloc'd, never stack (the z-buffer alone is 512 KB).
 * Zero-initialised before main runs; the core rewrites every byte of fb and
 * zb on every call anyway (forma.exsc fills the ground and zeroes zb before
 * drawing), so the .bss guarantee is belt-and-braces, not load-bearing. */
static uint8_t g_exsg[KILN_SOFT3D_EXSG_BYTES];
static uint8_t g_fb[KILN_SOFT3D_FB_BYTES];
static double  g_zb[256 * 256];

static int g_ready; /* kiln_soft3d_init ran; render is callable */

void kiln_soft3d_init(const void *exsg_blob, uint32_t len)
{
    assertf(exsg_blob != NULL, "kiln_soft3d_init: NULL stream");
    assertf(len == KILN_SOFT3D_EXSG_BYTES,
            "kiln_soft3d_init: stream is %lu bytes, the contract is %u",
            (unsigned long)len, KILN_SOFT3D_EXSG_BYTES);
    memcpy(g_exsg, exsg_blob, KILN_SOFT3D_EXSG_BYTES);
    g_ready = 1;
}

uint8_t kiln_soft3d_render_frame(void)
{
    assertf(g_ready, "kiln_soft3d_render_frame before kiln_soft3d_init");
    /* THE CALLER'S STACK: exs_signaculum_pingue alone frames 36,176 bytes
     * (mips64-elf-gcc 14.4.0 -Os, -fstack-usage — gen/PROVENANCE.md). Call
     * this from a >= 49,152-byte kthread, never from main, and the demo's
     * painted-stack bar is the measurement that proves the choice. */
    return (uint8_t)exs_signaculum_pingue(g_exsg, g_fb,
                                          (unsigned char *)g_zb);
}

const uint8_t *kiln_soft3d_framebuffer(void)
{
    return g_fb;
}

void kiln_soft3d_present(surface_t *disp)
{
    assertf(disp != NULL && disp->buffer != NULL,
            "kiln_soft3d_present: no surface");
    assertf(disp->width == 320 && disp->height == 240,
            "kiln_soft3d_present: the mapping is fixed for 320x240, got %dx%d",
            disp->width, disp->height);

    /* The crop and the letterbox, fixed: source rows [8, 248) land at
     * dst rows [0, 240), source columns [0, 256) land at dst columns
     * [32, 288), and everything outside is opaque black. Top-bit extraction
     * only — the frame on screen is a deterministic function of the
     * framebuffer's bytes, so two runs of the same stream never differ by
     * a pixel. */
    const bool wide = disp->stride >= (uint16_t)(disp->width * 4);
    assertf(wide || disp->stride >= (uint16_t)(disp->width * 2),
            "kiln_soft3d_present: stride %u cannot hold %u-wide pixels",
            disp->stride, disp->width);

    for (int y = 0; y < 240; y++) {
        const uint8_t *src = g_fb + (size_t)(y + 8) * 256 * 3;
        uint8_t *row = (uint8_t *)disp->buffer + (size_t)y * disp->stride;

        if (wide) {
            /* plat/host's display: RGBA8888, four bytes a pixel. */
            for (int x = 0; x < 32; x++) {
                row[x * 4 + 0] = 0; row[x * 4 + 1] = 0;
                row[x * 4 + 2] = 0; row[x * 4 + 3] = 255;
                row[(288 + x) * 4 + 0] = 0; row[(288 + x) * 4 + 1] = 0;
                row[(288 + x) * 4 + 2] = 0; row[(288 + x) * 4 + 3] = 255;
            }
            for (int x = 0; x < 256; x++) {
                uint8_t *p = row + (32 + x) * 4;
                p[0] = src[x * 3 + 0];
                p[1] = src[x * 3 + 1];
                p[2] = src[x * 3 + 2];
                p[3] = 255;
            }
        } else {
            /* The console's DEPTH_16_BPP: RGBA5551 in a big-endian u16
             * frame. (r>>3)<<11 | (g>>3)<<6 | (b>>3)<<1 | 1 — libdragon's
             * RGB16() arithmetic written out, so the pack is visible next
             * to the crop it shares the loop with. */
            uint16_t *row16 = (uint16_t *)row;
            for (int x = 0; x < 32; x++) {
                row16[x] = 0x0001;             /* black, alpha 1 */
                row16[288 + x] = 0x0001;
            }
            for (int x = 0; x < 256; x++) {
                const uint16_t r = src[x * 3 + 0] >> 3;
                const uint16_t g = src[x * 3 + 1] >> 3;
                const uint16_t b = src[x * 3 + 2] >> 3;
                row16[32 + x] = (uint16_t)((r << 11) | (g << 6) | (b << 1) | 1);
            }
        }
    }

    /* The VI DMAs this surface straight out of RDRAM; the CPU wrote it
     * through the data cache. Push every line out now — on the console this
     * is the whole reason the frame shows at all, on the host the shim's
     * no-op (there is one coherent view of memory there). */
    data_cache_hit_writeback(disp->buffer, (unsigned long)disp->stride * disp->height);
}
