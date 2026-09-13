/* SPDX-License-Identifier: MIT
 *
 * kiln_console.c — retro on-screen debug console. See kiln_console.h for the
 * design rationale.
 *
 * Layout (320×240, the engine's standard resolution):
 *
 *   Closed:  ┌──────────────┐  top-right corner
 *            │ [00.0] hello │  4-line log tail, always visible
 *            │ [00.1] ...   │
 *            └──────────────┘
 *
 *   Open:    ┌──────────────────────────────────────────┐  y=96, h=144
 *            │ log: scrolling, last 7 lines              │
 *            │                                            │
 *            │ ┌─┬─┬─┬─┬─┬─┬─┬─┬─┬─┐  keyboard 4×10    │
 *            │ │A│B│C│D│E│F│G│H│I│J│  stick to move,  │
 *            │ └─┴─┴─┴─┴─┴─┴─┴─┴─┴─┘  A to type       │
 *            │ > cmd_ █                                  │
 *            └──────────────────────────────────────────┘
 *
 * The chord (hold Start + C-Up → C-Left → C-Down → C-Right) toggles open.
 * Submit on Start fires too but is a no-op when the command line is empty,
 * so the user can hold Start to begin the close-chord without accidentally
 * submitting a half-typed buffer.
 */
#include "kiln_console.h"
#include "kiln_input.h"

#include <libdragon.h>
#include <kiln/kiln_actor.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>   /* KILN_BTN_* button bits */
#include <kiln/kiln_prof.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* The engine's frame loop is 320×240 (see kiln_engine.h / RESOLUTION_320x240
 * in every example). The console is sized against that width directly rather
 * than including a screen-dimensions header, so the module stays independent
 * of any future resolution knob. */
#define CON_SCREEN_W 320

/* ── Constants ──────────────────────────────────────────────────────────── */

#define CON_LOG_LINES 16
#define CON_LOG_LEN   64

#define CON_TAIL_X 188
#define CON_TAIL_Y 4
#define CON_TAIL_W 128
#define CON_TAIL_H 50
#define CON_TAIL_ROWS 4

#define CON_PANEL_Y 96
#define CON_PANEL_H 144

#define CON_LOG_Y     100
#define CON_LOG_ROWS  7

#define CON_KB_X      40
#define CON_KB_Y      174
#define CON_KB_COLS   10
#define CON_KB_ROWS   4
#define CON_KB_CELL_W 24
#define CON_KB_CELL_H 12

#define CON_CMD_Y     220

#define CON_CMD_MAX   64

/* ── State ──────────────────────────────────────────────────────────────── */

static char  g_log[CON_LOG_LINES][CON_LOG_LEN];
static int   g_log_head;   /* next slot to write */
static int   g_log_count; /* live entries (capped at CON_LOG_LINES) */

static bool     g_open;
static int      g_chord_step;
static uint32_t g_init_ticks;

static int g_cur_col, g_cur_row;
static int g_stick_state; /* 0=centered, 1=U, 2=R, 3=D, 4=L */

static char g_cmd[CON_CMD_MAX];
static int  g_cmd_len;

static int g_frame; /* cursor blink */

#define CON_CMD_CAP 16
static const KilnConsoleCmd *g_cmds[CON_CMD_CAP];
static size_t g_cmd_count;

/* Uppercase labels — the debug font renders uppercase cleanly. The dispatcher
 * lowercases the command line before matching, so the keyboard's case is
 * cosmetic. Row 3 has digits and a few symbols; the last cell is space. */
static const char KB_LABELS[CON_KB_ROWS][CON_KB_COLS] = {
    { 'A','B','C','D','E','F','G','H','I','J' },
    { 'K','L','M','N','O','P','Q','R','S','T' },
    { 'U','V','W','X','Y','Z','0','1','2','3' },
    { '4','5','6','7','8','9','_','.',' ','?' },
};

/* ── Log ────────────────────────────────────────────────────────────────── */

static void log_push(const char *s)
{
    char *line = g_log[g_log_head];
    snprintf(line, CON_LOG_LEN, "%s", s);
    g_log_head = (g_log_head + 1) % CON_LOG_LINES;
    if (g_log_count < CON_LOG_LINES) g_log_count++;
}

void kiln_console_log(const char *fmt, ...)
{
    char line[CON_LOG_LEN];
    uint32_t elapsed = TICKS_SINCE(g_init_ticks);
    float sec = (float)TICKS_TO_US(elapsed) / 1.0e6f;
    /* Write the timestamp prefix first, then append the user's message into
     * whatever space remains. Avoids the gcc -Wformat-truncation warning
     * you'd get from `snprintf(line, N, "[%4.1f] %s", sec, buf)` when `buf`
     * can be up to N-1 bytes — the compiler can't prove the prefix leaves
     * room. This form keeps the bound obvious. */
    int prefix = snprintf(line, sizeof line, "[%4.1f] ", sec);
    if (prefix < 0 || prefix >= (int)sizeof line) prefix = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + prefix, sizeof line - prefix, fmt, ap);
    va_end(ap);
    log_push(line);
}

/* ── Dispatch ─────────────────────────────────────────────────────────── */

static void cmd_help(int argc, const char **argv)
{
    (void)argc; (void)argv;
    kiln_console_log("commands:");
    kiln_console_log("  help  clear  exit");
    for (size_t i = 0; i < g_cmd_count; i++)
        kiln_console_log("  %s  %s", g_cmds[i]->name,
                        g_cmds[i]->help ? g_cmds[i]->help : "");
}

static void cmd_clear(int argc, const char **argv)
{
    (void)argc; (void)argv;
    g_log_count = 0; g_log_head = 0;
}

static void cmd_exit(int argc, const char **argv)
{
    (void)argc; (void)argv;
    g_open = false;
    kiln_console_log("console closed");
}

static void cmd_actors(int argc, const char **argv)
{
    (void)argc; (void)argv;
    uint16_t total = kiln_actor_count(KILN_ACTOR_CATEGORY_COUNT);
    if (total == 0 && kiln_actor_count(KILN_ACTOR_CAT_PLAYER) == 0) {
        kiln_console_log("actors: not initialised");
        return;
    }
    kiln_console_log("player %u  enemy %u  npc %u  prop %u  item %u  total %u",
                    kiln_actor_count(KILN_ACTOR_CAT_PLAYER),
                    kiln_actor_count(KILN_ACTOR_CAT_ENEMY),
                    kiln_actor_count(KILN_ACTOR_CAT_NPC),
                    kiln_actor_count(KILN_ACTOR_CAT_PROP),
                    kiln_actor_count(KILN_ACTOR_CAT_ITEM),
                    total);
}

static void cmd_prof(int argc, const char **argv)
{
    (void)argc; (void)argv;
    kiln_prof_print();
}

static const KilnConsoleCmd BUILTIN[] = {
    { "help",   "list commands",     cmd_help   },
    { "clear",  "clear the log",     cmd_clear  },
    { "exit",   "close the console", cmd_exit   },
    { "actors", "actor counts",      cmd_actors },
    { "prof",   "profiler summary",  cmd_prof   },
};

static void dispatch(char *line)
{
    /* Trim leading spaces. */
    while (*line == ' ') line++;
    if (*line == '\0') return; /* empty submit = no-op */

    /* Tokenise in place. */
    const char *argv[8];
    int argc = 0;
    char *p = line;
    while (*p && argc < 8) {
        while (*p == ' ') *p++ = '\0';
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }
    if (argc == 0) return;

    /* Lowercase argv[0] in place for case-insensitive matching. */
    for (char *q = (char *)argv[0]; *q; q++)
        if (*q >= 'A' && *q <= 'Z') *q += 'a' - 'A';

    /* Built-ins first. */
    for (size_t i = 0; i < sizeof BUILTIN / sizeof BUILTIN[0]; i++)
        if (strcmp(argv[0], BUILTIN[i].name) == 0) {
            BUILTIN[i].fn(argc, argv);
            return;
        }
    /* User-registered. */
    for (size_t i = 0; i < g_cmd_count; i++)
        if (strcmp(argv[0], g_cmds[i]->name) == 0) {
            g_cmds[i]->fn(argc, argv);
            return;
        }
    kiln_console_log("unknown: %s", argv[0]);
}

/* ── Init / register ─────────────────────────────────────────────────── */

void kiln_console_init(void)
{
    g_log_count = 0; g_log_head = 0;
    g_open = false;
    g_chord_step = 0;
    g_cur_col = 0; g_cur_row = 0;
    g_stick_state = 0;
    g_cmd_len = 0;
    g_cmd_count = 0;
    g_frame = 0;
    g_init_ticks = TICKS_READ();
}

void kiln_console_register(const KilnConsoleCmd *cmds, size_t n)
{
    for (size_t i = 0; i < n && g_cmd_count < CON_CMD_CAP; i++)
        g_cmds[g_cmd_count++] = &cmds[i];
}

bool kiln_console_is_open(void) { return g_open; }

int kiln_console_tail_lines(void)
{
    return g_log_count < CON_LOG_LINES ? g_log_count : CON_LOG_LINES;
}

const char *kiln_console_tail_line(int idx)
{
    int n = kiln_console_tail_lines();
    if (idx < 0 || idx >= n) return "";
    int start = (g_log_head - g_log_count + CON_LOG_LINES) % CON_LOG_LINES;
    int real = (start + idx) % CON_LOG_LINES;
    return g_log[real];
}

/* ── Input: chord + keyboard ─────────────────────────────────────────── */

static const uint32_t C_SEQ[4] = { KILN_BTN_CU, KILN_BTN_CL,
                                   KILN_BTN_CD,  KILN_BTN_CR };

static void poll_chord(uint32_t held, uint32_t pressed)
{
    /* Start must be held throughout the chord. */
    if (!(held & KILN_BTN_START)) {
        g_chord_step = 0;
        return;
    }
    /* Any C-button press this frame. */
    uint32_t c_any = pressed & (KILN_BTN_CU | KILN_BTN_CL
                                | KILN_BTN_CD | KILN_BTN_CR);
    if (!c_any) return;

    if (pressed & C_SEQ[g_chord_step]) {
        if (++g_chord_step == 4) {
            g_open = !g_open;
            g_chord_step = 0;
            kiln_console_log(g_open ? "console open" : "console closed");
        }
    } else {
        /* Wrong C button — restart. */
        g_chord_step = 0;
    }
}

static void poll_stick(int stick_x, int stick_y)
{
    /* Discrete stick navigation: move one cell when the stick crosses the
     * threshold in a direction it wasn't last frame. A 32-unit deadzone
     * matches the engine's kiln_input default. */
    int dx = 0, dy = 0;
    if (stick_x >  32) dx =  1;
    if (stick_x < -32) dx = -1;
    if (stick_y >  32) dy =  1;
    if (stick_y < -32) dy = -1;

    int new_state = 0;
    if (dy > 0) new_state = 1;       /* up    */
    else if (dx > 0) new_state = 2;  /* right */
    else if (dy < 0) new_state = 3;  /* down  */
    else if (dx < 0) new_state = 4;  /* left  */

    if (new_state == 0) {
        g_stick_state = 0;
        return;
    }
    /* Only advance on a fresh direction edge. */
    if (new_state != g_stick_state) {
        g_stick_state = new_state;
        switch (new_state) {
        case 1: g_cur_row = (g_cur_row + CON_KB_ROWS - 1) % CON_KB_ROWS; break;
        case 3: g_cur_row = (g_cur_row + 1) % CON_KB_ROWS;              break;
        case 2: g_cur_col = (g_cur_col + 1) % CON_KB_COLS;              break;
        case 4: g_cur_col = (g_cur_col + CON_KB_COLS - 1) % CON_KB_COLS; break;
        }
    }
}

static void type_cell(void)
{
    if (g_cmd_len >= CON_CMD_MAX - 1) return;
    char ch = KB_LABELS[g_cur_row][g_cur_col];
    if (ch == '?') return; /* placeholder, ignore */
    g_cmd[g_cmd_len++] = ch;
}

static void backspace(void) { if (g_cmd_len > 0) g_cmd_len--; }

void kiln_console_exec(const char *line)
{
    /* dispatch() tokenises in place, so it always gets a private copy. */
    char buf[CON_CMD_MAX];
    snprintf(buf, sizeof buf, "%s", line ? line : "");
    if (buf[0] == '\0') return;
    kiln_console_log("> %s", buf);
    dispatch(buf);
}

static void submit(void)
{
    if (g_cmd_len == 0) return; /* empty submit is a no-op */
    g_cmd[g_cmd_len] = '\0';
    g_cmd_len = 0;
    kiln_console_exec(g_cmd);
}

void kiln_console_update(int port)
{
    /* Through kiln_input, not the joypad. This read joypad_get_buttons and
     * diffed its own edges, so a kiln_input tape — an attract mode, or a jump
     * ROM entering the chord for a capture — could never open the console or
     * type into it: scripted input stopped at kiln_input and the console never
     * saw it. Every caller already runs kiln_input_update first each frame. */
    const KilnInput *in = kiln_input_get(port <= 0 ? 1 : port);
    const uint32_t held = in->buttons;
    const uint32_t pressed = in->edges;

    poll_chord(held, pressed);

    if (!g_open) return;

    /* Keyboard input. A = type, B = backspace, Start = submit. */
    if (pressed & KILN_BTN_A)  type_cell();
    if (pressed & KILN_BTN_B)  backspace();
    if (pressed & KILN_BTN_START) submit();

    poll_stick((int)(in->stick_x * (float)JOYPAD_RANGE_N64_STICK_MAX),
               (int)(in->stick_y * (float)JOYPAD_RANGE_N64_STICK_MAX));
}

/* ── Draw ──────────────────────────────────────────────────────────────── */

static const color_t C_BORDER = { .r = 0,   .g = 245, .b = 212, .a = 255 };
static const color_t C_FILL   = { .r = 10,  .g = 10,  .b = 24,  .a = 220 };
static const color_t C_TEXT    = { .r = 232, .g = 232, .b = 240, .a = 255 };
static const color_t C_ACCENT  = { .r = 0,   .g = 245, .b = 212, .a = 255 };
static const color_t C_CUR     = { .r = 245, .g = 0,   .b = 80,  .a = 255 };

static const char *log_at(int from_bottom)
{
    if (from_bottom >= g_log_count) return "";
    int idx = (g_log_head - 1 - from_bottom + CON_LOG_LINES) % CON_LOG_LINES;
    return g_log[idx];
}

static void draw_tail(void)
{
    if (g_log_count == 0) return;
    kiln_gui_panel(CON_TAIL_X, CON_TAIL_Y, CON_TAIL_W, CON_TAIL_H,
                  C_FILL, C_BORDER);
    for (int i = 0; i < CON_TAIL_ROWS; i++) {
        const char *line = log_at(CON_TAIL_ROWS - 1 - i);
        if (line[0])
            kiln_gui_text(CON_TAIL_X + 4, CON_TAIL_Y + 10 + i * 10,
                         C_TEXT, "%s", line);
    }
}

static void draw_log_panel(void)
{
    kiln_gui_panel(0, CON_PANEL_Y, CON_SCREEN_W, CON_PANEL_H, C_FILL, C_BORDER);
    kiln_gui_text(8, CON_LOG_Y, C_ACCENT, "log");
    for (int i = 0; i < CON_LOG_ROWS; i++) {
        const char *line = log_at(CON_LOG_ROWS - 1 - i);
        if (line[0])
            kiln_gui_text(8, CON_LOG_Y + 12 + i * 10, C_TEXT, "%s", line);
    }
}

static void draw_keyboard(void)
{
    for (int r = 0; r < CON_KB_ROWS; r++) {
        for (int c = 0; c < CON_KB_COLS; c++) {
            int x = CON_KB_X + c * CON_KB_CELL_W;
            int y = CON_KB_Y + r * CON_KB_CELL_H;
            int active = (r == g_cur_row && c == g_cur_col);
            color_t fill = active ? C_CUR : C_FILL;
            color_t txt  = active ? C_FILL : C_TEXT;
            color_t border = active ? C_CUR : C_BORDER;
            kiln_gui_panel(x, y, CON_KB_CELL_W - 2, CON_KB_CELL_H - 2,
                          fill, border);
            char label[2] = { KB_LABELS[r][c], '\0' };
            kiln_gui_text(x + 8, y + 2, txt, "%s", label);
        }
    }
}

static void draw_cmdline(void)
{
    kiln_gui_text(8, CON_CMD_Y, C_ACCENT, ">");
    /* Render whatever's typed. The cursor blinks at the end of the line. */
    char buf[CON_CMD_MAX + 2];
    memcpy(buf, g_cmd, g_cmd_len);
    buf[g_cmd_len] = '\0';
    kiln_gui_text(20, CON_CMD_Y, C_TEXT, "%s", buf);
    if ((g_frame / 30) % 2 == 0) {
        int cx = 20 + g_cmd_len * 8;
        kiln_gui_rect(cx, CON_CMD_Y, 8, 9, C_ACCENT);
    }
    kiln_gui_text(CON_SCREEN_W - 116, CON_CMD_Y, C_TEXT,
                 "A: type  B: bk  Start: send");
}

void kiln_console_draw(void)
{
    g_frame++;
    if (!g_open) {
        draw_tail();
        return;
    }
    draw_log_panel();
    draw_keyboard();
    draw_cmdline();
}