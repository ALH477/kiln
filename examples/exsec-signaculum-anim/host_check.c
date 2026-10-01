/* SPDX-License-Identifier: MIT
 *
 * host_check.c -- the animation's correctness gate, on the HOST tier: the real
 * engine/src/kiln/kiln_soft3d.c compiled against plat/host's <libdragon.h>,
 * driving the PINNED engine/src/kiln/gen/signaculum_x86_64.gen.c unit through
 * all 28 rotation-matrix patches plus the unpatched hero view, and comparing
 * every framebuffer CRC-32 against crc_pins.h.
 *
 * WHY HOST AND NOT ARES. The project's answer for "did the engine draw the
 * right pixels" is the host tier -- nix/checks/kiln-maprender.nix and
 * kiln-splash.nix compile the real module, render, and diff a committed
 * reference, with no ROM and no compositor. The soft3d groundwork for that
 * was already laid: the x86_64 gen unit is pinned beside the mips64 one and
 * kiln_soft3d.c's `#if defined(N64)` switch exists to choose between them.
 * The ROM's in-ROM CRC is the same gate; this is a transport for it that
 * runs in a sandbox.
 *
 * Build and run (from the repository root):
 *
 *   cc -O2 -I plat/host/include -I engine/src/kiln -I engine/include \
 *      -o /tmp/host_check examples/exsec-signaculum-anim/host_check.c \
 *      engine/src/kiln/kiln_soft3d.c
 *   /tmp/host_check examples/exsec-signaculum-anim/filesystem/signaculum.exsg \
 *                   examples/exsec-signaculum-anim/filesystem/rotations.bin
 *
 * Do NOT also pass the gen unit to the compiler: kiln_soft3d.c #includes it
 * (its header says so), and linking it twice is a multiple-definition error.
 *
 * Exits non-zero if any of the 29 views disagrees, so it is usable as a check
 * as it stands.
 */
#include "kiln_soft3d.h"
#include "crc_pins.h"
#include <stdio.h>
/* the host libdragon shim macro-replaces stdio; this TU does its own file IO */
#undef fopen
#undef fread
#undef fclose
#undef printf
#undef puts
#undef fprintf
#include <string.h>
#include <stdlib.h>

_Noreturn void exsrt_abortus(unsigned kind)
{ fprintf(stderr, "exsrt_abortus(%u)\n", kind); exit(9); }

static uint32_t crc32_iso(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) { c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (~(c & 1) + 1u)); }
    return c ^ 0xFFFFFFFFu;
}
static uint8_t exsg[FIG_SOFT3D_EXSG_BYTES];
static uint8_t rot[28 * FIG_SOFT3D_ROT_BYTES];

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(exsg, 1, sizeof exsg, f) != sizeof exsg) { puts("bad stream"); return 2; }
    fclose(f);
    f = fopen(argv[2], "rb");
    if (!f || fread(rot, 1, sizeof rot, f) != sizeof rot) { puts("bad rotation table"); return 2; }
    fclose(f);

    fig_soft3d_init(exsg, sizeof exsg);
    /* SEQUENCED, deliberately: render_frame and framebuffer() must not be two
     * arguments of one call -- C leaves argument evaluation order
     * unspecified, and taking the CRC of the not-yet-rendered buffer is
     * exactly the mistake that produced a bogus hero digest here once. */
    int bad = 0;
    uint8_t hst = fig_soft3d_render_frame();
    uint32_t hcrc = crc32_iso(fig_soft3d_framebuffer(), FIG_SOFT3D_FB_BYTES);
    printf("hero (unpatched): status=%u crc=%#010x  %s\n", hst, hcrc,
           hcrc == SIG_HERO_CRC ? "AGREE (pinned EXPECTED_CRC)" : "DISAGREE");
    if (hst != FIG_SOFT3D_OK || hcrc != SIG_HERO_CRC) bad++;
    for (int i = 0; i < 28; i++) {
        fig_soft3d_set_rotation(rot + (size_t)i * FIG_SOFT3D_ROT_BYTES, FIG_SOFT3D_ROT_BYTES);
        uint8_t st = fig_soft3d_render_frame();
        uint32_t c = crc32_iso(fig_soft3d_framebuffer(), FIG_SOFT3D_FB_BYTES);
        int ok = (st == FIG_SOFT3D_OK) && (c == sig_frame_crc[i]);
        if (!ok) bad++;
        printf("frame %2d: status=%u crc=%#010x  %s\n", i, st, c,
               ok ? "AGREE" : "DISAGREE");
    }
    if (bad) printf("\nDISAGREE: %d of %u views\n", bad, SIG_ANIM_FRAMES + 1u);
    else     printf("\nAGREE: all %u views (hero + %u animation frames)\n",
                    SIG_ANIM_FRAMES + 1u, SIG_ANIM_FRAMES);
    return bad ? 1 : 0;
}
