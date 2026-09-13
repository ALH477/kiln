// SPDX-License-Identifier: MIT
//
// Every example in this repo on one cartridge, behind a menu.
//
// HOW A DEMO RUNS HERE. Each example's source is compiled unchanged, with
// `-Dmain=demo_<id>_main` (see the Makefile), and linked into this one ELF --
// `main` is the only global the examples have in common. The menu calls the
// chosen one and never gets control back: no example returns from main, and
// none shuts down what it started. RESET is a fresh boot, so RESET is the way
// back, and it lands here.
//
// WHY THE MENU DRAWS WITH graphics.h AND NOT kiln_gui. Almost every example
// begins with kiln_engine_init, whose display_init asserts if a display is
// already open, and which also starts rdpq, Tiny3D and the GUI font -- which
// nothing in this tree ever starts twice in one boot (kiln_gui.c keeps its
// font across a close for exactly that reason). So the menu opens a display
// and nothing else, draws into it on the CPU, and closes it before the demo
// starts. The demo then sees the machine a cold boot would have given it, plus
// a joypad subsystem that is already running -- and joypad_init is
// reference-counted, so its own call is harmless.
//
// AUTORUN. Built with AUTORUN=<id> (flake.nix's demo-reel-<id>), the menu draws
// itself for half a second and then launches that demo through the same
// hand-over as pressing A. That is the jump ROM AGENTS.md prefers to
// ./dev drive, and it exercises the hand-over, which is the only new code path.

#include <libdragon.h>
#include <string.h>

typedef struct {
    const char *id;
    const char *label;
    const char *blurb;
    int (*run)(void);
} Demo;

#define DEMO(id, dir, src, label, blurb) int demo_##id##_main(void);
#include "demos.def"
#undef DEMO

static const Demo DEMOS[] = {
#define DEMO(id, dir, src, label, blurb) { #id, label, blurb, demo_##id##_main },
#include "demos.def"
#undef DEMO
};
#define NDEMOS ((int)(sizeof DEMOS / sizeof DEMOS[0]))

#define LIST_Y  34
#define ROW_H   10
#define FOOT_Y  204
#define ROWS    ((FOOT_Y - LIST_Y) / ROW_H)

static int autorun_index(void)
{
#ifdef REEL_AUTORUN
    for (int i = 0; i < NDEMOS; i++)
        if (strcmp(DEMOS[i].id, REEL_AUTORUN) == 0) return i;
    assertf(0, "demo-reel: AUTORUN=%s is not in demos.def", REEL_AUTORUN);
#endif
    return -1;
}

int main(void)
{
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    joypad_init();

    const uint32_t bg     = graphics_make_color(10, 10, 24, 255);
    const uint32_t head   = graphics_make_color(0, 245, 212, 255);
    const uint32_t ink    = graphics_make_color(232, 232, 240, 255);
    const uint32_t dim    = graphics_make_color(130, 130, 150, 255);
    const uint32_t hilite = graphics_make_color(40, 60, 110, 255);

    const int autorun = autorun_index();
    int sel = autorun >= 0 ? autorun : 0;
    int top = 0;
    int shown = 0;

    for (;;) {
        joypad_poll();
        joypad_buttons_t p = joypad_get_buttons_pressed(JOYPAD_PORT_1);
        int dy = joypad_get_axis_pressed(JOYPAD_PORT_1, JOYPAD_AXIS_STICK_Y);
        int go = 0;

        if (autorun < 0) {
            if (p.d_up || dy > 0)   sel = (sel + NDEMOS - 1) % NDEMOS;
            if (p.d_down || dy < 0) sel = (sel + 1) % NDEMOS;
            if (p.l) sel = sel >= ROWS ? sel - ROWS : 0;
            if (p.r) sel = sel + ROWS < NDEMOS ? sel + ROWS : NDEMOS - 1;
            go = p.a || p.start;
        } else {
            go = shown >= 30;
        }
        if (sel < top) top = sel;
        if (sel >= top + ROWS) top = sel - ROWS + 1;

        surface_t *disp = display_get();
        graphics_fill_screen(disp, bg);

        graphics_set_color(head, 0);
        graphics_draw_text(disp, 12, 12, "KILN DEMO REEL");
        char count[16];
        snprintf(count, sizeof count, "%d demos", NDEMOS);
        graphics_set_color(dim, 0);
        graphics_draw_text(disp, 308 - 8 * (int)strlen(count), 12, count);
        graphics_draw_box(disp, 12, 24, 296, 1, head);

        for (int r = 0; r < ROWS && top + r < NDEMOS; r++) {
            const int i = top + r;
            const int y = LIST_Y + r * ROW_H;
            if (i == sel) graphics_draw_box(disp, 8, y - 1, 304, ROW_H, hilite);
            char line[48];
            snprintf(line, sizeof line, "%2d  %s", i + 1, DEMOS[i].label);
            graphics_set_color(i == sel ? head : ink, 0);
            graphics_draw_text(disp, 14, y, line);
        }
        graphics_set_color(dim, 0);
        if (top > 0)               graphics_draw_text(disp, 300, LIST_Y, "^");
        if (top + ROWS < NDEMOS)   graphics_draw_text(disp, 300, LIST_Y + (ROWS - 1) * ROW_H, "v");

        graphics_draw_box(disp, 12, FOOT_Y, 296, 1, head);
        graphics_set_color(ink, 0);
        graphics_draw_text(disp, 12, FOOT_Y + 8, DEMOS[sel].blurb);
        graphics_set_color(dim, 0);
        graphics_draw_text(disp, 12, FOOT_Y + 22,
                           autorun >= 0 ? "AUTORUN" : "A run   L/R page   RESET: back here");
        display_show(disp);
        shown++;

        if (go) break;
    }

    // THE HAND-OVER. Close the display the menu opened, so the demo's own
    // display_init (kiln_engine_init's, or console_init's) finds none open.
    // Nothing else the menu started needs closing: see the header.
    display_close();
    return DEMOS[sel].run();
}
