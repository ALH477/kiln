/* SPDX-License-Identifier: MIT
 *
 * kiln_soft3d.h — the Exsecutor logo rasterizer as Kiln's built-in software
 * 3D path: a CPU rasterizer baked into the engine as the tiny3d-free answer
 * for a ROM that wants to draw a real shaded mesh without touching the RSP.
 *
 * The core is NOT written here. `exs_signaculum_pingue` is Exsecutor code —
 * a pure library function compiled OUT OF `examples/signaculum/forma.exsc` by
 * the Exsecutor compiler's C backend, byte-identical behaviour certified in
 * that repo's own suite (see gen/PROVENANCE.md for the commit, the commands
 * and the digests). This module owns the storage the pure core borrows and
 * maps its 256×256 RGB888 framebuffer onto a Kiln display surface.
 *
 * ── Storage ────────────────────────────────────────────────────────────
 * The core takes everything as caller-owned buffers (Exsecutor's ADR 0016
 * mutable-borrow discipline), and this module IS that caller: the EXSG
 * stream (44,801 B), the framebuffer (196,608 B) and the f64 1/z z-buffer
 * (524,288 B) are three static .bss arrays here, ~706 KB in total. No heap,
 * init-once, deterministic.
 *
 * ── The stack is the caller's problem, and it is not small ─────────────
 * kiln_soft3d_render_frame's callee frame is 36,176 bytes on
 * mips64-elf-gcc 14.4.0 -mabi=o64 -march=vr4300 -Os (-fstack-usage;
 * gen/PROVENANCE.md). Do NOT call it from main: libdragon's main thread
 * stack would survive it, but the intent is the precedent set by
 * examples/exsec-streamdb-demo — run the render on a dedicated kthread of
 * 49,152 bytes (exsec-signaculum-demo does, and measures the high-water
 * mark against the painted stack).
 *
 * ── What is NOT guessed ────────────────────────────────────────────────
 * The prototypes below are hand-written against c-backend.md's library-mode
 * ABI (every integer uint64_t, every address unsigned char *), and are
 * CHECKED, not trusted: kiln_soft3d.c includes this header and then the
 * generated unit itself, so a disagreement is a conflicting-types compile
 * error; examples/exsec-signaculum-demo's exsec_proto_check.c repeats that
 * pair as a compiled-never-linked TU, the exsec-streamdb-demo pattern.
 *
 * ── Gen unit selection ─────────────────────────────────────────────────
 * There are two pinned units and the mips64 one refuses a 64-bit-pointer
 * compile (_Static_assert). kiln_soft3d.c picks with `#if defined(N64)` —
 * the macro libdragon's n64.mk defines for the console tier and no host
 * build defines. The tree deliberately carries no other host/console #ifdef
 * (nix/host.nix's header: "no #ifdef anywhere in the engine"); this include
 * switch is the first one, and it is about which generated unit feeds the
 * compiler, not about behaviour.
 */
#ifndef KILN_SOFT3D_H
#define KILN_SOFT3D_H

#include <libdragon.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Byte counts of the wire contract, from forma.exsc's signature. */
#define KILN_SOFT3D_EXSG_BYTES 44801u    /* acies<u8, 44801>  */
#define KILN_SOFT3D_FB_BYTES   196608u   /* 256 * 256 * 3     */
#define KILN_SOFT3D_SIDE       256u      /* the source frame is square */

/** signaculum_pingue's return codes (forma.exsc's own header comment). */
enum {
    KILN_SOFT3D_OK             = 0,
    KILN_SOFT3D_BAD_MAGIC      = 1,      /* not "EXSG"                    */
    KILN_SOFT3D_BAD_COUNTS     = 2,      /* not the 1493/2981 this bakes  */
    KILN_SOFT3D_SHORT_STREAM   = 3,      /* a decode read ran past the end */
    KILN_SOFT3D_TRAILING_BYTES = 4,      /* decode did not end at 44801   */
};

/** The pure core, emitted by exsc. Prototype by hand, checked by inclusion
 *  (see the header comment); p0 is the 44,801-byte EXSG stream, p1 the
 *  196,608-byte RGB888 framebuffer, p2 the 524,288-byte f64 z-buffer. */
uint64_t exs_signaculum_pingue(unsigned char *p0, unsigned char *p1,
                               unsigned char *p2);

/** The ONE symbol the generated unit imports. Whoever links the render
 *  supplies the body (examples/exsec-signaculum-demo/main.c does). */
_Noreturn void exsrt_abortus(unsigned kind);

/** Copy `len` bytes of an EXSG stream into the module's storage. Asserts on
 *  anything but exactly KILN_SOFT3D_EXSG_BYTES — the core's decode checks
 *  are belt-and-braces against a well-formed stream, not a size discovery
 *  protocol. Does not render. */
void kiln_soft3d_init(const void *exsg_blob, uint32_t len);

/** Rasterize the logo: clear fb/zb and draw the full mesh into the module's
 *  own storage. Returns signaculum_pingue's status (KILN_SOFT3D_OK on
 *  success). THE FRAME IS 36,176 BYTES — see the header comment. */
uint8_t kiln_soft3d_render_frame(void);

/** Read-only access to the 196,608-byte RGB888 framebuffer, for the demo's
 *  in-ROM CRC self-check. Valid after kiln_soft3d_render_frame. */
const uint8_t *kiln_soft3d_framebuffer(void);

/** Map the 256×256 RGB888 frame into `disp`, a 320×240 display surface:
 *  32 px of opaque-black letterbox on each side, 8 rows cropped top and
 *  bottom (256 − 240), colours packed by top bits — a deterministic shift,
 *  no dither, no rounding. Handles both surface widths the engine has:
 *  RGBA5551 (u16, (r>>3)<<11|(g>>3)<<6|(b>>3)<<1|1, the console's
 *  DEPTH_16_BPP) and RGBA8888 (the plat/host shims' display).
 *
 *  Ends with the data-cache writeback the VI needs before scanout: the CPU
 *  wrote every pixel through the CPU's view of RDRAM, and neither the VI
 *  nor the RDP reads that view. plat/host's no-op copy of
 *  data_cache_hit_writeback is the correct host implementation, not a
 *  missing one. */
void kiln_soft3d_present(surface_t *disp);

#ifdef __cplusplus
}
#endif

#endif /* KILN_SOFT3D_H */
