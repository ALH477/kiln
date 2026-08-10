// SPDX-License-Identifier: MPL-2.0
//
// uipreview — render m64_widget's screens on the host, to a PNG-able PPM.
//
//     make -C tools/uipreview && tools/uipreview/uipreview out-prefix
//
// ── Why this exists ────────────────────────────────────────────────────────
// Every other visual decision in this repo is checked by rendering it:
// models go through Blender, geometry goes through the signed-volume tests.
// The 2D layer had nothing. `./dev shot` is the intended answer and it does
// not work in every environment (CLAUDE.md documents an Ares whose Vulkan
// surface never composites), which left the menus as the one thing being
// designed blind — and a UI whose whole brief is "off-kilter" is exactly the
// thing you cannot tune without looking at it.
//
// m64_widget.c happens to be trivially portable: it calls nothing but
// m64_gui's four primitives and fm_sinf. So it compiles natively against two
// small shim headers and the four primitives implemented here as a software
// rasteriser. What you see is the real widget code doing its real layout
// arithmetic — not a mock-up of it.
//
// ── What it is NOT ─────────────────────────────────────────────────────────
// Not an emulator, and not a substitute for `./dev shot`. It knows nothing
// about the RDP: no alpha-blend ordering quirks, no scissor, no 16-bit
// colour, no CPU cost. It answers "is the layout right and does it look
// good", which is the question the design work needs, and leaves "does it
// draw correctly on hardware" to the console.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "m64_widget.h"

#define W 320
#define H 240
#define SCALE 3

static uint8_t fb[H][W][3];

// ── the four m64_gui primitives, in software ──────────────────────────────

static void blend(int x, int y, color_t c)
{
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    float a = c.a / 255.0f;
    fb[y][x][0] = (uint8_t)(fb[y][x][0] * (1 - a) + c.r * a);
    fb[y][x][1] = (uint8_t)(fb[y][x][1] * (1 - a) + c.g * a);
    fb[y][x][2] = (uint8_t)(fb[y][x][2] * (1 - a) + c.b * a);
}

void m64_gui_rect(int x, int y, int w, int h, color_t c)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            blend(x + i, y + j, c);
}

void m64_gui_panel(int x, int y, int w, int h, color_t fill, color_t border)
{
    m64_gui_rect(x, y, w, h, border);
    if (w > 2 && h > 2) m64_gui_rect(x + 1, y + 1, w - 2, h - 2, fill);
}

void m64_gui_bar(int x, int y, int w, int h, float frac, color_t fg,
                 color_t bg)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    m64_gui_rect(x, y, w, h, bg);
    m64_gui_rect(x, y, (int)(w * frac), h, fg);
}

void m64_gui_init(void) {}
void m64_gui_close(void) {}
void m64_gui_begin(void) {}
void m64_gui_end(void) {}

// ── a 3x5 font ────────────────────────────────────────────────────────────
// Enough to read the labels back. Not libdragon's built-in debug font — that
// one lives in the ROM and has different metrics — so glyph SHAPES here are
// indicative and glyph ADVANCE is exact (M64_WIDGET_CHAR_W), which is the
// half that layout depends on.
static const char *glyph(char c)
{
    switch (c) {
    case 'A': return "111101111101101";  case 'B': return "110101110101110";
    case 'C': return "111100100100111";  case 'D': return "110101101101110";
    case 'E': return "111100111100111";  case 'F': return "111100111100100";
    case 'G': return "111100101101111";  case 'H': return "101101111101101";
    case 'I': return "111010010010111";  case 'J': return "001001001101111";
    case 'K': return "101101110101101";  case 'L': return "100100100100111";
    case 'M': return "101111111101101";  case 'N': return "110101101101101";
    case 'O': return "111101101101111";  case 'P': return "111101111100100";
    case 'Q': return "111101101111001";  case 'R': return "111101110101101";
    case 'S': return "111100111001111";  case 'T': return "111010010010010";
    case 'U': return "101101101101111";  case 'V': return "101101101101010";
    case 'W': return "101101111111101";  case 'X': return "101101010101101";
    case 'Y': return "101101010010010";  case 'Z': return "111001010100111";
    case '0': return "111101101101111";  case '1': return "010110010010111";
    case '2': return "111001111100111";  case '3': return "111001111001111";
    case '4': return "101101111001001";  case '5': return "111100111001111";
    case '6': return "111100111101111";  case '7': return "111001001001001";
    case '8': return "111101111101111";  case '9': return "111101111001111";
    case '.': return "000000000000010";  case '-': return "000000111000000";
    case ':': return "000010000010000";  case '!': return "010010010000010";
    case '/': return "001001010100100";  case '>': return "100010001010100";
    case '<': return "001010100010001";  case '?': return "111001010000010";
    case '%': return "101001010100101";  case '+': return "000010111010000";
    default:  return NULL;
    }
}

void m64_gui_text(int x, int y, color_t c, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    // m64_gui_text takes a BASELINE; the glyphs hang above it.
    for (int i = 0; buf[i]; i++) {
        char ch = buf[i];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        const char *g = glyph(ch);
        if (!g) continue;
        int gx = x + i * M64_WIDGET_CHAR_W;
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (g[row * 3 + col] == '1')
                    m64_gui_rect(gx + col * 2, y - 10 + row * 2, 2, 2, c);
    }
}

// ── output ────────────────────────────────────────────────────────────────

static void clear(color_t c)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            fb[y][x][0] = c.r; fb[y][x][1] = c.g; fb[y][x][2] = c.b;
        }
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", W * SCALE, H * SCALE);
    for (int y = 0; y < H; y++)
        for (int sy = 0; sy < SCALE; sy++)
            for (int x = 0; x < W; x++)
                for (int sx = 0; sx < SCALE; sx++)
                    fwrite(fb[y][x], 1, 3, f);
    fclose(f);
    printf("  wrote %s\n", path);
}

// ── the screens ───────────────────────────────────────────────────────────
// Deliberately NOT gg_screens.c: that file is game code and pulls in the
// board, the turn machine and the actor system. These are the same widget
// calls with the same style, which is what the design pass needs to see.

static const char *const TITLE_ITEMS[] = {
    "START MATCH", "AUTO DEMO", "HOW TO PLAY",
};
static const char *const GOBLINS[] = { "DANK", "SPARKY", "MOSS", "GLIMMER" };

static void motes(const M64WidgetStyle *st)
{
    float t = m64_widget_time();
    for (int i = 0; i < 12; i++) {
        float ax = 0.7f + 0.5f * m64_widget_jitter(i * 3u + 1u);
        float ay = 0.5f + 0.4f * m64_widget_jitter(i * 3u + 2u);
        float ph = m64_widget_jitter(i * 3u + 5u) * 3.1416f;
        int x = (int)(W * 0.5f + sinf(t * 0.11f * ax + ph) * W * 0.46f);
        int y = (int)(H * 0.5f + cosf(t * 0.09f * ay + ph * 1.7f) * H * 0.44f);
        int sz = 2 + (i % 3);
        color_t c = (i & 1) ? st->accent : st->border;
        m64_gui_rect(x, y, sz, sz, RGBA32(c.r, c.g, c.b, 40 + (i % 3) * 14));
    }
}

static void wobble_title(int cx, int y, const char *text,
                         const M64WidgetStyle *st, float amp, float rate)
{
    float t = m64_widget_time();
    int len = (int)strlen(text);
    int x = cx - len * M64_WIDGET_CHAR_W / 2;
    for (int i = 0; i < len; i++) {
        if (text[i] == ' ') continue;
        float ph = (float)i * 0.55f;
        int dy = (int)(amp * sinf(t * rate * 6.2831853f + ph));
        int dx = (int)(amp * 0.35f * cosf(t * rate * 4.4f + ph * 1.3f));
        m64_gui_text(x + i * M64_WIDGET_CHAR_W + dx, y + dy, st->accent,
                     "%c", text[i]);
    }
}

static void screen_title(const M64WidgetStyle *st, M64Menu *menu)
{
    motes(st);
    m64_widget_panel_skew(38, 26, W - 76, 48, -st->lean * 1.6f,
                          st->bg, st->accent);
    wobble_title(W / 2, 52, "GANJA GOBLIN", st, 3.2f, 0.30f);
    m64_gui_text(W / 2 - 11 * M64_WIDGET_CHAR_W, 68, st->dim,
                 "A HARVEST FOR FOUR GOBLINS");
    m64_menu_draw(menu, 98, 100, 124, TITLE_ITEMS, NULL, st);
    m64_gui_text(W / 2 - 13 * M64_WIDGET_CHAR_W, H - 14, st->dim,
                 "D-PAD MOVE   A CONFIRM   B BACK");
}

static void screen_select(const M64WidgetStyle *st, M64Menu *menu)
{
    static const color_t tint[4] = {
        { 0, 245, 120, 255 }, { 255, 90, 190, 255 },
        { 255, 190, 60, 255 }, { 90, 200, 255, 255 },
    };
    float t = m64_widget_time();
    motes(st);
    m64_widget_panel_skew(-4, -2, W + 8, 20, -st->lean, st->bg, st->border);
    m64_gui_text(8, 13, st->accent, "PLAYER 2 - PICK YOUR GOBLIN");
    int pulse = (int)(2.0f * sinf(t * 5.0f));
    m64_gui_rect(W - 22 - pulse, 3 - pulse, 12 + pulse * 2, 10 + pulse * 2,
                 tint[1]);

    uint8_t avail[4] = { 0, 1, 1, 1 };
    m64_menu_draw(menu, 10, 30, 122, GOBLINS, avail, st);

    int cx = 142 + (int)(1.8f * sinf(t * 1.9f));
    int cy = 30 + (int)(1.4f * cosf(t * 1.5f));
    m64_widget_panel_skew(cx, cy, W - cx - 8, 98, -st->lean * 1.2f,
                          st->bg, tint[1]);
    wobble_title(cx + (W - cx - 8) / 2, cy + 16, "SPARKY", st, 1.8f, 0.55f);
    m64_gui_text(cx + 8, cy + 34, st->text, "PASSIVE");
    m64_gui_text(cx + 8, cy + 46, st->dim, "MOVE +1 EVERY 3RD");
    m64_gui_text(cx + 6, cy + 64, st->text, "SPECIAL");
    m64_gui_text(cx + 6, cy + 76, st->dim, "SPARK PLUG");
    m64_gui_text(cx + 4, cy + 92, st->warn, "CHARGE: 12 BUDS");

    for (int p = 0; p < 4; p++) {
        int bx = 12 + p * 76 + (int)(st->jitter * m64_widget_jitter(p + 40u));
        int by = 176 + (int)(st->jitter * 0.8f * m64_widget_jitter(p + 60u));
        int taken = (p < 1);
        color_t edge = taken ? tint[p] : st->dim;
        m64_widget_panel_skew(bx, by, 68, 28,
                              (p & 1) ? st->lean : -st->lean, st->bg, edge);
        m64_gui_text(bx + 6, by + 12, taken ? st->text : st->dim, "P%d", p + 1);
        m64_gui_text(bx + 6, by + 24, taken ? edge : st->dim,
                     "%s", taken ? GOBLINS[p] : "...");
    }
    m64_gui_text(W / 2 - 13 * M64_WIDGET_CHAR_W, H - 12, st->dim,
                 "A LOCK IN    B BACK A PLAYER");
}

static void screen_results(const M64WidgetStyle *st, M64Menu *menu)
{
    static const color_t tint[4] = {
        { 0, 245, 120, 255 }, { 255, 90, 190, 255 },
        { 255, 190, 60, 255 }, { 90, 200, 255, 255 },
    };
    M64PlayerSlot slots[4];
    static const char *names[4] = { "DANK", "SPARKY", "MOSS", "GLIMMER" };
    static const int32_t score[4] = { 31, 47, 22, 39 };
    for (int i = 0; i < 4; i++) {
        slots[i] = (M64PlayerSlot){ .name = names[i], .note = NULL,
                                    .score = score[i], .charge = -1.0f,
                                    .tint = tint[i], .active = 0, .ready = 0 };
    }
    int order[4] = { 1, 3, 0, 2 };
    motes(st);
    wobble_title(W / 2, 26, "SPARKY", st, 3.6f, 0.42f);
    m64_gui_text(W / 2 - 2 * M64_WIDGET_CHAR_W, 38, st->text, "WINS");
    m64_widget_results(46, 46, W - 92, "FINAL", slots, order, 4, st);
    static const char *const again[] = { "REMATCH", "BACK TO TITLE" };
    m64_menu_draw(menu, 86, 152, 148, again, NULL, st);
}

static void screen_hud(const M64WidgetStyle *st)
{
    static const color_t tint[4] = {
        { 0, 245, 120, 255 }, { 255, 90, 190, 255 },
        { 255, 190, 60, 255 }, { 90, 200, 255, 255 },
    };
    M64PlayerSlot slots[4];
    static const char *names[4] = { "DANK", "SPARKY", "MOSS", "GLIMMER" };
    for (int i = 0; i < 4; i++) {
        slots[i] = (M64PlayerSlot){
            .name = names[i], .note = (i == 2) ? "COUCH" : NULL,
            .score = 12 + i * 7, .charge = 0.2f + i * 0.26f,
            .tint = tint[i], .active = (i == 1), .ready = (i == 3),
        };
    }
    m64_gui_panel(0, 0, W, 16, st->bg, st->border);
    m64_gui_text(4, 12, st->accent, "GANJA GROVE");
    m64_gui_text(W - 15 * M64_WIDGET_CHAR_W, 12, st->text, "R 3/10  MOVE");
    m64_widget_hud_strip(4, 20, 150, slots, 4, st);
    m64_widget_dice(W - 40, 22, 32, 5, 0, 0.0f, st);
    m64_widget_banner(W / 2 - 92, 118, 184, 26, "HARVEST EVENT", 1.0f, st);
}

int main(int argc, char **argv)
{
    const char *prefix = (argc > 1) ? argv[1] : "ui";
    M64WidgetStyle funky = m64_widget_style_funky();
    M64WidgetStyle plain = m64_widget_style_default();
    color_t bg = RGBA32(14, 10, 26, 255);
    char path[512];

    struct { const char *name; void (*fn)(const M64WidgetStyle *, M64Menu *);
             int count, cursor; } screens[] = {
        { "title",   screen_title,   3, 1 },
        { "select",  screen_select,  4, 1 },
        { "results", screen_results, 2, 0 },
    };

    // A quarter of a second in, so the sway is off its zero crossing and the
    // motes have moved: a preview rendered at t=0 shows the one frame where
    // every animated offset happens to be zero, which is exactly the frame
    // that tells you nothing.
    m64_widget_tick(2.35f);

    for (size_t i = 0; i < sizeof screens / sizeof *screens; i++) {
        M64Menu menu;
        m64_menu_init(&menu, screens[i].count, 0);
        menu.cursor = screens[i].cursor;
        clear(bg);
        screens[i].fn(&funky, &menu);
        snprintf(path, sizeof path, "%s-%s.ppm", prefix, screens[i].name);
        write_ppm(path);
    }

    clear(bg);
    screen_hud(&funky);
    snprintf(path, sizeof path, "%s-hud.ppm", prefix);
    write_ppm(path);

    // The same title screen with the funk dialled to zero, as the control.
    // If these two are hard to tell apart, the funk is not doing anything.
    {
        M64Menu menu;
        m64_menu_init(&menu, 3, 0);
        menu.cursor = 1;
        clear(bg);
        screen_title(&plain, &menu);
        snprintf(path, sizeof path, "%s-title-plain.ppm", prefix);
        write_ppm(path);
    }
    return 0;
}
