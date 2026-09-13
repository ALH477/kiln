// SPDX-License-Identifier: MIT
//
// The smallest thing that proves the whole path works: cross toolchain ->
// libdragon -> n64tool -> a .z64 that boots and draws. Plain libdragon, no Kiln
// engine, so it is also the template for a project that wants neither.
//
// Colour bars and a bouncing "Kiln" block through rdpq, paced by the VI:
// display_get() blocks until a buffer is free, which is once per frame, so the
// frame counter on screen advances at 60 per second and no faster.

#include <libdragon.h>

#define W 320
#define H 240

static const color_t BARS[7] = {
    { 192, 192, 192, 255 }, { 192, 192, 0, 255 }, { 0, 192, 192, 255 },
    { 0, 192, 0, 255 },     { 192, 0, 192, 255 }, { 192, 0, 0, 255 },
    { 0, 0, 192, 255 },
};

int main(void)
{
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE);
    rdpq_init();
    rdpq_text_register_font(FONT_BUILTIN_DEBUG_MONO,
                            rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO));

    // debugf() goes to the host over flashcart USB when one is attached, and
    // is a no-op otherwise. This is the channel `./dev debug` reads.
    debug_init_isviewer();
    debugf("Kiln: hello from the VR4300\n");

    int x = 40, y = 60, dx = 1, dy = 1;
    for (uint32_t frame = 0;; frame++) {
        rdpq_attach_clear(display_get(), NULL);

        for (int i = 0; i < 7; i++) {
            rdpq_set_mode_fill(BARS[i]);
            rdpq_fill_rectangle(i * W / 7, 0, (i + 1) * W / 7, 160);
        }
        for (int i = 0; i < 16; i++) {           // a grey ramp under the bars
            rdpq_set_mode_fill(RGBA32(i * 17, i * 17, i * 17, 255));
            rdpq_fill_rectangle(i * W / 16, 160, (i + 1) * W / 16, 200);
        }

        x += dx; y += dy;                        // one pixel per frame
        if (x <= 0 || x >= W - 56) dx = -dx;
        if (y <= 0 || y >= 200 - 24) dy = -dy;
        rdpq_set_mode_fill(RGBA32(16, 16, 32, 255));
        rdpq_fill_rectangle(x, y, x + 56, y + 24);
        rdpq_text_printf(NULL, FONT_BUILTIN_DEBUG_MONO, x + 16, y + 16, "Kiln");

        rdpq_text_printf(NULL, FONT_BUILTIN_DEBUG_MONO, 12, 218,
                         "libdragon on a Nix-built mips64-elf toolchain");
        rdpq_text_printf(NULL, FONT_BUILTIN_DEBUG_MONO, 12, 232, "frame %lu",
                         (unsigned long)frame);
        rdpq_detach_show();
    }
}
