/* SPDX-License-Identifier: MIT
 *
 * fig_console's input, asserted on the host with the real kiln_console.c,
 * kiln_input.c and the host joypad.
 *
 * The console read joypad_get_buttons directly, so fig_input's tapes — the
 * mechanism every example uses for attract modes and for jump ROMs that must
 * reach a state with no controller — could never open it. debug-demo's CONSOLE
 * jump played the chord and nothing happened. This pins both paths: a real pad
 * opens and closes the console, and a scripted chord opens it too.
 */
#include <kiln_console.h>
#include <kiln_input.h>
#include <libdragon.h>

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const uint16_t CHORD_BTNS[] = {
    FIG_BTN_START, FIG_BTN_START | FIG_BTN_CU, FIG_BTN_START,
    FIG_BTN_START | FIG_BTN_CL, FIG_BTN_START, FIG_BTN_START | FIG_BTN_CD,
    FIG_BTN_START, FIG_BTN_START | FIG_BTN_CR, 0,
};

static void frame(uint16_t real_buttons)
{
    joypad_inputs_t pad = { 0 };
    pad.btn.raw = real_buttons;
    fig_host_pad_set(JOYPAD_PORT_1, pad);
    fig_input_update();
    fig_console_update(1);
}

static void real_chord(void)
{
    for (size_t i = 0; i < sizeof CHORD_BTNS / sizeof CHORD_BTNS[0]; i++)
        for (int f = 0; f < 3; f++) frame(CHORD_BTNS[i]);
}

static const FigInputKey TAPE_KEYS[] = {
    { .frame =  0, .buttons = FIG_BTN_START },
    { .frame =  4, .buttons = FIG_BTN_START | FIG_BTN_CU },
    { .frame =  7, .buttons = FIG_BTN_START },
    { .frame = 10, .buttons = FIG_BTN_START | FIG_BTN_CL },
    { .frame = 13, .buttons = FIG_BTN_START },
    { .frame = 16, .buttons = FIG_BTN_START | FIG_BTN_CD },
    { .frame = 19, .buttons = FIG_BTN_START },
    { .frame = 22, .buttons = FIG_BTN_START | FIG_BTN_CR },
    { .frame = 25 },
};
static const FigInputTape TAPE = { TAPE_KEYS, 9, FIG_INPUT_NO_LOOP };

static int tail_has(const char *needle)
{
    for (int i = 0; i < fig_console_tail_lines(); i++)
        if (strstr(fig_console_tail_line(i), needle)) return 1;
    return 0;
}

int main(void)
{
    joypad_init();
    fig_input_init();
    fig_console_init();

    real_chord();
    CHECK(fig_console_is_open(), "the chord on a real pad did not open the console");
    real_chord();
    CHECK(!fig_console_is_open(), "the chord on a real pad did not close the console again");

    fig_input_play(1, &TAPE);
    for (int f = 0; f < 40; f++) frame(0);
    CHECK(fig_console_is_open(),
          "a fig_input tape playing the chord did not open the console — scripted "
          "input is not reaching it");
    CHECK(tail_has("console open"), "the console did not log that it opened");

    if (fails) { printf("\nFAILED (%d)\n", fails); return 1; }
    printf("fig_console: a real pad and a fig_input tape both drive the chord\n");
    return 0;
}
