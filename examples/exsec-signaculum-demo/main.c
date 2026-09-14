// SPDX-License-Identifier: MIT
//
// The Exsecutor logo, rasterized by Exsecutor code running on Kiln — the
// signaculum pure core (examples/signaculum/forma.exsc, emitted as
// engine/src/kiln/gen/signaculum_mips64.gen.c; PROVENANCE.md there has the
// pin) behind the kiln_soft3d engine module, on a 49,152-byte kthread.
//
// WHAT THIS ROM SHOWS:
//
//   1. The EXSG stream (rom:/signaculum.exsg, 44,801 bytes — Exsecutor's
//      tests/data/signaculum_mesh.bin, byte for byte) handed to the pure
//      core, which decodes the 1,493-vertex / 2,981-face fixed-point mesh
//      and rasterizes it at 256x256 RGB888 with an f64 1/z z-buffer.
//   2. THE IN-ROM CERTIFICATE: a CRC-32 (ISO-HDLC — the polynomial family
//      the StreamDB container format uses) over the 196,608 framebuffer
//      bytes, against the constant baked from Exsecutor's own
//      tests/programs/signaculum/expected.out on x86-64 and qemu-musl-mips64.
//      MIPS r3 FPU flush-to-zero on subnormals is exactly the class of
//      difference that qemu's soft-fp hides, so AGREE here is earned on the
//      VR4300, not inherited from the cross suite. debugf prints the verdict.
//   3. The frame presented as the engine's built-in software 3D path:
//      kiln_soft3d_present maps 256x256 RGB888 into the 320x240 RGBA5551
//      display surface — 32 px letterbox each side, 8 rows cropped top and
//      bottom — with the data-cache writeback scanout needs.
//   4. The kthread's real high-water mark against the static -fstack-usage
//      figure, measured the way examples/exsec-streamdb-demo does it: paint
//      the stack, run the render, scan for the paint afterwards.
//
// There is nothing to drive: the render is one shot, the present loop is the
// frame pacing, and the verdict is in the upper panel from frame one.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_soft3d.h>

#include <string.h>

#define EXSG_PATH     "rom:/signaculum.exsg"
#define RENDER_STACK  49152u   /* 36,176-byte core frame + margin */
#define STATIC_FRAME  36176u   /* gen/PROVENANCE.md: -fstack-usage */
#define STACK_PAINT   0xA5

/* CRC-32, ISO-HDLC. Computed over the Exsecutor repo's
 * tests/programs/signaculum/expected.out pixel payload (its last 196,608
 * bytes — the 15-byte "P6\n256 256\n255\n" header excluded), from a
 * checkout at the pinned commit a413e6f:
 *
 *   python3 - <<'EOF'
 *   import zlib, pathlib
 *   p = pathlib.Path('tests/programs/signaculum/expected.out').read_bytes()
 *   assert p[:15] == b'P6\n256 256\n255\n' and len(p) == 196623
 *   print(f"{zlib.crc32(p[-196608:]) & 0xFFFFFFFF:#010x}")
 *   EOF
 *
 * zlib.crc32 IS CRC-32/ISO-HDLC (poly 0xEDB88320, reflected, init and
 * xorout 0xFFFFFFFF) — the crc32_iso_hld below reproduces it bit by bit. */
#define EXPECTED_CRC  0x2025C173u

static uint8_t g_exsg[KILN_SOFT3D_EXSG_BYTES];

/* CRC-32/ISO-HDLC, bitwise. Table-free on purpose: this runs ONCE on a
 * finished render, and 196,608 bytes x 8 steps costs the console well under
 * a second; a 1 KB table would be boot cost and an extra thing to verify. */
static uint32_t crc32_iso_hldlc(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1) + 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

// The generated unit's one import. A trap inside Exsecutor code — a bounds
// or overflow check — lands here with its abort kind.
_Noreturn void exsrt_abortus(unsigned kind)
{
    assertf(0, "exsecutor: abortus %u", kind);
    for (;;) { }
}

typedef struct {
    int      ran;        // the thread body executed before kthread_new returned
    uint8_t  status;     // signaculum_pingue's KILN_SOFT3D_* code
    uint32_t crc;        // CRC over the module's framebuffer
} RenderResult;

// Paint the unused part of this thread's stack, from the bottom up to a
// margin below this function's own frame. libdragon puts the kthread_t at
// the TOP of the stack allocation, so the stack is exactly
// [kthread_current() - RENDER_STACK, kthread_current()).
//
// ADDRESSES, NOT POINTERS — see the same comment in exsec-streamdb-demo's
// main.c: as pointer arithmetic this loop's bound is undefined and GCC
// compiled it into an unconditional branch that painted the heap. As
// uintptr_t it is ordinary arithmetic. The margin covers memset's own frame;
// an interrupt taken meanwhile writes its 576-byte register dump below sp,
// which can only make a byte read as used, never the reverse.
__attribute__((noinline))
static void paint_stack(void)
{
    const uintptr_t base = (uintptr_t)kthread_current() - RENDER_STACK;
    const uintptr_t stop = (uintptr_t)__builtin_frame_address(0) - 1024;
    if (stop > base) memset((void *)base, STACK_PAINT, stop - base);
}

static int render(void *arg)
{
    RenderResult *r = arg;
    paint_stack();
    r->ran = 1;
    r->status = kiln_soft3d_render_frame();
    if (r->status == KILN_SOFT3D_OK)
        r->crc = crc32_iso_hldlc(kiln_soft3d_framebuffer(), KILN_SOFT3D_FB_BYTES);
    else
        r->crc = 0;
    return 0;
}

int main(void)
{
    kernel_init();
    kiln_engine_init(RESOLUTION_320x240);
    debug_init_isviewer();
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);

    // ---- the stream, into the module's storage ----
    FILE *f = fopen(EXSG_PATH, "rb");
    assertf(f, "exsec-signaculum-demo: %s not found", EXSG_PATH);
    uint32_t len = (uint32_t)fread(g_exsg, 1, sizeof g_exsg, f);
    int more = fgetc(f);
    fclose(f);
    assertf(more == EOF && len == KILN_SOFT3D_EXSG_BYTES,
            "exsec-signaculum-demo: stream is not %u bytes", KILN_SOFT3D_EXSG_BYTES);
    kiln_soft3d_init(g_exsg, len);

    // ---- the render, on a 48 KiB thread ----
    // Priority +1, ABOVE main, so the thread runs to completion inside
    // kthread_new (libdragon kernel.c: a new thread at >= the current
    // priority is switched to immediately), and only then is it joined.
    // kthread_join's blocking path mis-records the joiner and every join of
    // a still-running thread asserts in the Inspector — the reason
    // exsec-streamdb-demo runs its reader at +1; the same constraint applies
    // here, with a bigger stack: 36,176 bytes of callee frame does not fit
    // the reader demo's 32,768.
    RenderResult res = { .status = 255 };
    kthread_t *th = kthread_new("signaculum", RENDER_STACK, 1, render, &res);
    assertf(th, "exsec-signaculum-demo: kthread_new failed");

    // The finished thread's memory is still allocated until the join, so its
    // stack reads here: the first byte above the bottom that is no longer
    // paint is the deepest the render reached.
    uint32_t stack_peak = 0;
    if (res.ran) {
        const uintptr_t top = (uintptr_t)th;
        uintptr_t a = top - RENDER_STACK;
        while (a < top && *(const unsigned char *)a == STACK_PAINT) a++;
        stack_peak = (uint32_t)(top - a);
    }
    kthread_join(th);

    const int agree = res.status == KILN_SOFT3D_OK && res.crc == EXPECTED_CRC;
    debugf("signaculum: %s status=%u crc=0x%08lx want=0x%08lx stack=%lu/%u\n",
           agree ? "AGREE" : "DISAGREE", res.status, (unsigned long)res.crc,
           (unsigned long)EXPECTED_CRC, (unsigned long)stack_peak, RENDER_STACK);

    // ---- present, one mapped frame per display buffer ----
    const color_t INK  = { 232, 232, 240, 255 };
    const color_t HEAD = { 0, 245, 212, 255 };
    const color_t GOOD = { 80, 230, 120, 255 };
    const color_t BAD  = { 255, 90, 90, 255 };
    const color_t DIM  = { 144, 152, 176, 255 };
    const color_t FILL = { 10, 10, 24, 255 };

    for (;;) {
        surface_t *surf = display_get();

        /* The software 3D pass: CPU-writes the whole surface, then pushes
         * the lines out to RDRAM — VI and (below) the RDP read them. Order
         * matters: nothing CPU-side may write this surface again before
         * show. */
        kiln_soft3d_present(surf);

        /* A 2D status pass over the same surface. No 3D pass, no
         * t3d_frame_start — kiln_gui_begin is rdpq-only. */
        rdpq_attach(surf, NULL);
        kiln_gui_begin();

        kiln_gui_panel(4, 4, 312, 40, FILL, HEAD);
        kiln_gui_text(10, 17, HEAD, "EXSECUTOR SIGNACULUM — KILN SOFT3D");
        kiln_gui_text(10, 30, DIM, "%u verts, %u faces, f64 1/z, CPU", 1493u, 2981u);
        kiln_gui_text(204, 30, agree ? GOOD : BAD, "%s  %08lx",
                      agree ? "AGREE" : "DISAGREE", (unsigned long)res.crc);

        kiln_gui_panel(4, 188, 312, 34, FILL, HEAD);
        kiln_gui_text(10, 201, INK, "status %u   crc %08lx / %08lx", res.status,
                      (unsigned long)res.crc, (unsigned long)EXPECTED_CRC);
        kiln_gui_text(10, 214, stack_peak > RENDER_STACK - 2048 ? BAD : DIM,
                      "stack %lu/%u (static %u)", (unsigned long)stack_peak,
                      RENDER_STACK, STATIC_FRAME);

        kiln_gui_end();
        rdpq_detach_show();
    }
}
