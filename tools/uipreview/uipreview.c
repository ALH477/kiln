// SPDX-License-Identifier: MIT
//
// uipreview — render kiln_widget's screens on the host, to a PNG-able PPM.
//
//     make -C tools/uipreview && tools/uipreview/uipreview out-prefix
//
// ── Why this exists ──────────────────────────────────────────────
// Every other visual decision in this repo is checked by rendering it. The 2D
// layer had nothing: `./dev shot` is the intended answer and it does not work
// in every environment (CLAUDE.md documents an Ares whose Vulkan surface never
// composites), which left the menus as the one thing being designed blind —
// and a UI whose whole brief is "off-kilter" is exactly the thing you cannot
// tune without looking at it.
//
// ── It used to draw its own pixels, and that was the bug ────────────────
// This harness originally implemented kiln_gui's four primitives itself, over
// a private <libdragon.h> shim and a hand-rolled 3x5 font. The layout was
// real, because kiln_widget.c was real — but the drawing was a second
// implementation, and it disagreed with the first in three ways that all
// mattered to exactly the judgement this tool exists to support:
//
//   * panel   drew the border first and inset the body. The real one draws
//             the body and then four 1px edges, because a translucent body
//             over a solid border tints the border.
//   * bar     filled from the very edge at full height. The real one insets
//             by a pixel on all four sides.
//   * alpha   blended every rect. The console does NOT: with the blender off
//             — which is where kiln_gui_begin leaves it — the RDP ignores
//             source alpha and writes opaque. So motes(), which asks for
//             alpha 40-68, is solid squares on hardware. That is worth
//             seeing, and this tool used to hide it.
//
// It now links the REAL engine/src/kiln/kiln_gui.c against the shared host
// backend in plat/host/, and draws with libdragon's OWN builtin font, decoded
// out of the blob the ROM links (tools/font_extract.py). So glyph shapes and
// advances are the console's, not indicative. One host rasteriser, one font,
// one set of primitives.
//
// ── What it is NOT ─────────────────────────────────────────────
// Not an emulator, and not a substitute for `./dev shot`. It knows nothing
// about scissoring, 16-bit colour dithering, or fill rate — the console's
// actual binding constraint, which has no host analogue at all. It answers
// "is the layout right and does it look good", and leaves "is it affordable"
// to the console.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include <libdragon.h>
#include <kiln_host.h>

#include "kiln_gui.h"
#include "kiln_widget.h"

#define W 320
#define H 240

static void clear(color_t c)
{
    /* Through the real kiln_gui, like everything else here. */
    kiln_gui_rect(0, 0, W, H, c);
}

static void capture(const char *path)
{
    if (kiln_host_capture(path) != 0) { perror(path); exit(1); }
    printf("  wrote %s\n", path);
}

// ── the screens ───────────────────────────────────────────────────────────
// Deliberately NOT a real game's own screen code: that would pull in the
// board, the turn machine and the actor system just to look at a layout.
// These are the same widget calls with the same style, which is what the
// design pass needs to see.

static const char *const TITLE_ITEMS[] = {
    "START MATCH", "AUTO DEMO", "HOW TO PLAY",
};
static const char *const GOBLINS[] = { "DANK", "SPARKY", "MOSS", "GLIMMER" };

static void motes(const KilnWidgetStyle *st)
{
    float t = kiln_widget_time();
    for (int i = 0; i < 12; i++) {
        float ax = 0.7f + 0.5f * kiln_widget_jitter(i * 3u + 1u);
        float ay = 0.5f + 0.4f * kiln_widget_jitter(i * 3u + 2u);
        float ph = kiln_widget_jitter(i * 3u + 5u) * 3.1416f;
        int x = (int)(W * 0.5f + sinf(t * 0.11f * ax + ph) * W * 0.46f);
        int y = (int)(H * 0.5f + cosf(t * 0.09f * ay + ph * 1.7f) * H * 0.44f);
        int sz = 2 + (i % 3);
        color_t c = (i & 1) ? st->accent : st->border;
        kiln_gui_rect(x, y, sz, sz, RGBA32(c.r, c.g, c.b, 40 + (i % 3) * 14));
    }
}

static void wobble_title(int cx, int y, const char *text,
                         const KilnWidgetStyle *st, float amp, float rate)
{
    float t = kiln_widget_time();
    int len = (int)strlen(text);
    int x = cx - len * KILN_WIDGET_CHAR_W / 2;
    for (int i = 0; i < len; i++) {
        if (text[i] == ' ') continue;
        float ph = (float)i * 0.55f;
        int dy = (int)(amp * sinf(t * rate * 6.2831853f + ph));
        int dx = (int)(amp * 0.35f * cosf(t * rate * 4.4f + ph * 1.3f));
        kiln_gui_text(x + i * KILN_WIDGET_CHAR_W + dx, y + dy, st->accent,
                     "%c", text[i]);
    }
}

static void screen_title(const KilnWidgetStyle *st, KilnMenu *menu)
{
    motes(st);
    kiln_widget_panel_skew(38, 26, W - 76, 48, -st->lean * 1.6f,
                          st->bg, st->accent);
    wobble_title(W / 2, 52, "GOBLIN GROVE", st, 3.2f, 0.30f);
    kiln_gui_text(W / 2 - 11 * KILN_WIDGET_CHAR_W, 68, st->dim,
                 "A PARTY GAME FOR FOUR GOBLINS");
    kiln_menu_draw(menu, 98, 100, 124, TITLE_ITEMS, NULL, st);
    kiln_gui_text(W / 2 - 13 * KILN_WIDGET_CHAR_W, H - 14, st->dim,
                 "D-PAD MOVE   A CONFIRM   B BACK");
}

static void screen_select(const KilnWidgetStyle *st, KilnMenu *menu)
{
    static const color_t tint[4] = {
        { 0, 245, 120, 255 }, { 255, 90, 190, 255 },
        { 255, 190, 60, 255 }, { 90, 200, 255, 255 },
    };
    float t = kiln_widget_time();
    motes(st);
    kiln_widget_panel_skew(-4, -2, W + 8, 20, -st->lean, st->bg, st->border);
    kiln_gui_text(8, 13, st->accent, "PLAYER 2 - PICK YOUR GOBLIN");
    int pulse = (int)(2.0f * sinf(t * 5.0f));
    kiln_gui_rect(W - 22 - pulse, 3 - pulse, 12 + pulse * 2, 10 + pulse * 2,
                 tint[1]);

    uint8_t avail[4] = { 0, 1, 1, 1 };
    kiln_menu_draw(menu, 10, 30, 122, GOBLINS, avail, st);

    int cx = 142 + (int)(1.8f * sinf(t * 1.9f));
    int cy = 30 + (int)(1.4f * cosf(t * 1.5f));
    kiln_widget_panel_skew(cx, cy, W - cx - 8, 98, -st->lean * 1.2f,
                          st->bg, tint[1]);
    wobble_title(cx + (W - cx - 8) / 2, cy + 16, "SPARKY", st, 1.8f, 0.55f);
    kiln_gui_text(cx + 8, cy + 34, st->text, "PASSIVE");
    kiln_gui_text(cx + 8, cy + 46, st->dim, "MOVE +1 EVERY 3RD");
    kiln_gui_text(cx + 6, cy + 64, st->text, "SPECIAL");
    kiln_gui_text(cx + 6, cy + 76, st->dim, "QUICK CHARGE");
    kiln_gui_text(cx + 4, cy + 92, st->warn, "CHARGE: 12 PTS");

    for (int p = 0; p < 4; p++) {
        int bx = 12 + p * 76 + (int)(st->jitter * kiln_widget_jitter(p + 40u));
        int by = 176 + (int)(st->jitter * 0.8f * kiln_widget_jitter(p + 60u));
        int taken = (p < 1);
        color_t edge = taken ? tint[p] : st->dim;
        kiln_widget_panel_skew(bx, by, 68, 28,
                              (p & 1) ? st->lean : -st->lean, st->bg, edge);
        kiln_gui_text(bx + 6, by + 12, taken ? st->text : st->dim, "P%d", p + 1);
        kiln_gui_text(bx + 6, by + 24, taken ? edge : st->dim,
                     "%s", taken ? GOBLINS[p] : "...");
    }
    kiln_gui_text(W / 2 - 13 * KILN_WIDGET_CHAR_W, H - 12, st->dim,
                 "A LOCK IN    B BACK A PLAYER");
}

static void screen_results(const KilnWidgetStyle *st, KilnMenu *menu)
{
    static const color_t tint[4] = {
        { 0, 245, 120, 255 }, { 255, 90, 190, 255 },
        { 255, 190, 60, 255 }, { 90, 200, 255, 255 },
    };
    KilnPlayerSlot slots[4];
    static const char *names[4] = { "DANK", "SPARKY", "MOSS", "GLIMMER" };
    static const int32_t score[4] = { 31, 47, 22, 39 };
    for (int i = 0; i < 4; i++) {
        slots[i] = (KilnPlayerSlot){ .name = names[i], .note = NULL,
                                    .score = score[i], .charge = -1.0f,
                                    .tint = tint[i], .active = 0, .ready = 0 };
    }
    int order[4] = { 1, 3, 0, 2 };
    motes(st);
    wobble_title(W / 2, 26, "SPARKY", st, 3.6f, 0.42f);
    kiln_gui_text(W / 2 - 2 * KILN_WIDGET_CHAR_W, 38, st->text, "WINS");
    kiln_widget_results(46, 46, W - 92, "FINAL", slots, order, 4, st);
    static const char *const again[] = { "REMATCH", "BACK TO TITLE" };
    kiln_menu_draw(menu, 86, 152, 148, again, NULL, st);
}

static void screen_hud(const KilnWidgetStyle *st)
{
    static const color_t tint[4] = {
        { 0, 245, 120, 255 }, { 255, 90, 190, 255 },
        { 255, 190, 60, 255 }, { 90, 200, 255, 255 },
    };
    KilnPlayerSlot slots[4];
    static const char *names[4] = { "DANK", "SPARKY", "MOSS", "GLIMMER" };
    for (int i = 0; i < 4; i++) {
        slots[i] = (KilnPlayerSlot){
            .name = names[i], .note = (i == 2) ? "DIZZY" : NULL,
            .score = 12 + i * 7, .charge = 0.2f + i * 0.26f,
            .tint = tint[i], .active = (i == 1), .ready = (i == 3),
        };
    }
    kiln_gui_panel(0, 0, W, 16, st->bg, st->border);
    kiln_gui_text(4, 12, st->accent, "GOBLIN GROVE");
    kiln_gui_text(W - 15 * KILN_WIDGET_CHAR_W, 12, st->text, "R 3/10  MOVE");
    kiln_widget_hud_strip(4, 20, 150, slots, 4, st);
    kiln_widget_dice(W - 40, 22, 32, 5, 0, 0.0f, st);
    kiln_widget_banner(W / 2 - 92, 118, 184, 26, "BONUS EVENT", 1.0f, st);
}

int main(int argc, char **argv)
{
    const char *prefix = (argc > 1) ? argv[1] : "ui";
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    rdpq_init();
    kiln_gui_init();
    KilnWidgetStyle funky = kiln_widget_style_funky();
    KilnWidgetStyle plain = kiln_widget_style_default();
    color_t bg = RGBA32(14, 10, 26, 255);
    char path[512];

    struct { const char *name; void (*fn)(const KilnWidgetStyle *, KilnMenu *);
             int count, cursor; } screens[] = {
        { "title",   screen_title,   3, 1 },
        { "select",  screen_select,  4, 1 },
        { "results", screen_results, 2, 0 },
    };

    // A quarter of a second in, so the sway is off its zero crossing and the
    // motes have moved: a preview rendered at t=0 shows the one frame where
    // every animated offset happens to be zero, which is exactly the frame
    // that tells you nothing.
    kiln_widget_tick(2.35f);

    for (size_t i = 0; i < sizeof screens / sizeof *screens; i++) {
        KilnMenu menu;
        kiln_menu_init(&menu, screens[i].count, 0);
        menu.cursor = screens[i].cursor;
        rdpq_attach(display_get(), display_get_zbuf());
        kiln_gui_begin();
        clear(bg);
        screens[i].fn(&funky, &menu);
        kiln_gui_end();
        rdpq_detach_show();
        snprintf(path, sizeof path, "%s-%s.png", prefix, screens[i].name);
        capture(path);
    }

    rdpq_attach(display_get(), display_get_zbuf());
    kiln_gui_begin();
    clear(bg);
    screen_hud(&funky);
    kiln_gui_end();
    rdpq_detach_show();
    snprintf(path, sizeof path, "%s-hud.png", prefix);
    capture(path);

    // The same title screen with the funk dialled to zero, as the control.
    // If these two are hard to tell apart, the funk is not doing anything.
    {
        KilnMenu menu;
        kiln_menu_init(&menu, 3, 0);
        menu.cursor = 1;
        rdpq_attach(display_get(), display_get_zbuf());
        kiln_gui_begin();
        clear(bg);
        screen_title(&plain, &menu);
        kiln_gui_end();
        rdpq_detach_show();
        snprintf(path, sizeof path, "%s-title-plain.png", prefix);
        capture(path);
    }
    return 0;
}
