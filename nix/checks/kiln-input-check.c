/* SPDX-License-Identifier: MIT
 *
 * kiln_input's tapes, asserted on the host with the real kiln_input.c and the
 * host joypad (plat/host/src/host_io.c), which latches once per joypad_poll
 * exactly as libdragon does.
 *
 * Every demo's attract mode and every jump ROM stands on this, and its failures
 * are all of the "the demo just stands there" kind — indistinguishable in a
 * capture from a camera pointed the wrong way. So the frame arithmetic is
 * pinned here instead:
 *
 *   edges       a scripted press is an edge on its first frame only, held after,
 *               released on the frame the next key clears it
 *   loop        a looping tape re-presses at the loop point, so the edge fires
 *               again rather than the button reading held across the seam
 *   forced      kiln_input_play ignores the physical pad
 *   attract     takes over after exactly `idle_frames` idle updates, and ANY
 *               real input returns the port on that same frame
 *   untouched   a port with no tape reads the pad unchanged
 */
#include <kiln_input.h>
#include <libdragon.h>

#include <stdio.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void pad(int port, uint16_t buttons, int8_t sx)
{
    joypad_inputs_t in = { 0 };
    in.btn.raw = buttons;
    in.stick_x = sx;
    kiln_host_pad_set((joypad_port_t)(port - 1), in);
}

/* Press A for frames 3..5, stick right from 4, end at 10 and loop to 0. */
static const KilnInputKey PRESS_KEYS[] = {
    { .frame = 0 },
    { .frame = 3,  .buttons = KILN_BTN_A },
    { .frame = 4,  .buttons = KILN_BTN_A, .sx = 85 },
    { .frame = 6,  .sx = 85 },
    { .frame = 10 },
};
static const KilnInputTape PRESS = { PRESS_KEYS, 5, 0 };

static const KilnInputKey HOLD_KEYS[] = { { .frame = 0, .buttons = KILN_BTN_Z } };
static const KilnInputTape HOLD = { HOLD_KEYS, 1, KILN_INPUT_NO_LOOP };

int main(void)
{
    joypad_init();
    kiln_input_init();

    /* ── untouched ── */
    pad(1, KILN_BTN_B, 0);
    kiln_input_update();
    CHECK(kiln_input_pressed(1, KILN_BTN_B), "no tape: a real B press is not an edge");
    CHECK(!kiln_input_scripted(1), "no tape: port reports scripted");
    pad(1, 0, 0);
    kiln_input_update();

    /* ── forced, with the real pad held on something else ── */
    pad(1, KILN_BTN_START, -80);
    kiln_input_play(1, &PRESS);
    for (int f = 0; f < 24; f++) {
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);
        const int tf = f % 10;
        CHECK(kiln_input_scripted(1), "forced frame %d: not scripted", f);
        CHECK(!(in->buttons & KILN_BTN_START), "forced frame %d: the real pad leaked through", f);
        CHECK(!!(in->edges & KILN_BTN_A) == (tf == 3),
              "forced frame %d (tape %d): A edge %d", f, tf, !!(in->edges & KILN_BTN_A));
        CHECK(!!(in->buttons & KILN_BTN_A) == (tf >= 3 && tf <= 5),
              "forced frame %d (tape %d): A held %d", f, tf, !!(in->buttons & KILN_BTN_A));
        CHECK(!!(in->released & KILN_BTN_A) == (tf == 6),
              "forced frame %d (tape %d): A released %d", f, tf, !!(in->released & KILN_BTN_A));
        CHECK((in->stick_x > 0.9f) == (tf >= 4 && tf <= 9),
              "forced frame %d (tape %d): stick_x %.2f", f, tf, in->stick_x);
    }
    kiln_input_play(1, NULL);
    CHECK(!kiln_input_scripted(1), "play(NULL) did not stop the tape");
    kiln_input_update();
    CHECK(kiln_input_held(1, KILN_BTN_START), "after stopping, the real pad is not back");

    /* ── attract ── */
    pad(1, 0, 0);
    kiln_input_set_attract(1, &HOLD, 5);
    for (int f = 1; f <= 8; f++) {
        kiln_input_update();
        CHECK(kiln_input_scripted(1) == (f >= 5), "attract update %d: scripted %d",
              f, kiln_input_scripted(1));
        CHECK(kiln_input_held(1, KILN_BTN_Z) == (f >= 5), "attract update %d: Z held %d",
              f, kiln_input_held(1, KILN_BTN_Z));
    }
    /* A stick nudge inside the deadzone is not a person. */
    pad(1, 0, 5);
    kiln_input_update();
    CHECK(kiln_input_scripted(1), "a stick inside the deadzone cancelled attract");
    /* A real press cancels on the frame it happens. */
    pad(1, KILN_BTN_B, 0);
    kiln_input_update();
    CHECK(!kiln_input_scripted(1), "a real B press did not cancel attract");
    CHECK(kiln_input_pressed(1, KILN_BTN_B) && !kiln_input_held(1, KILN_BTN_Z),
          "the cancelling frame did not read the real pad");
    /* ...and the idle count restarts from zero. */
    pad(1, 0, 0);
    for (int f = 1; f <= 5; f++) {
        kiln_input_update();
        CHECK(kiln_input_scripted(1) == (f >= 5), "re-idle update %d: scripted %d",
              f, kiln_input_scripted(1));
    }

    /* ── ports are independent ── */
    CHECK(!kiln_input_scripted(2), "port 2 reports scripted with no tape");

    if (fails) { printf("\nFAILED (%d)\n", fails); return 1; }
    printf("kiln_input tapes: edges, loop, forced, attract and cancel all hold\n");
    return 0;
}
