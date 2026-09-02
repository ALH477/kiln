/* SPDX-License-Identifier: MIT
 *
 * kiln_input.c — see kiln_input.h for the model.
 */

#include "kiln_input.h"

#include <string.h>

static KilnInput g_input[JOYPAD_PORT_COUNT];
static uint32_t g_last_buttons[JOYPAD_PORT_COUNT];

void kiln_input_init(void)
{
    memset(g_input, 0, sizeof(g_input));
    memset(g_last_buttons, 0, sizeof(g_last_buttons));
}

void kiln_input_update(void)
{
    /* ── The poll this module is named for ──────────────────────────────
     * libdragon reads the joypads asynchronously under interrupt, and
     * `joypad_poll` is what synchronises that background state into what
     * `joypad_get_inputs` / `joypad_get_buttons` return (joypad.h:34, :468-470).
     * Without it those two report whatever was last synchronised, which for a
     * ROM that never polls is the zeroed initial state — forever.
     *
     * This call was missing, and the symptom is total: every button reads as
     * unheld, both sticks read centred, and no edge ever fires, so a ROM
     * builds and boots and runs at full frame rate and simply cannot be
     * played. It survived because nothing here can press a button —
     * `./dev shot` has no input path by design, `./dev drive`'s
     * uinput -> SDL -> ares chain is fragile enough that a dead pad reads as
     * the harness failing again, and every ROM that predates this module
     * called joypad_poll itself at the top of its own frame loop, so the
     * examples that were being watched kept working while everything built on
     * the wrapper did not.
     *
     * This header has claimed "kiln_input_update() polls once at the top of
     * the frame" since the module was written. It is now true. */
    joypad_poll();

    for (int p = 0; p < JOYPAD_PORT_COUNT; p++) {
        joypad_port_t port = (joypad_port_t)p;
        joypad_inputs_t in = joypad_get_inputs(port);
        joypad_buttons_t b = joypad_get_buttons(port);

        const float dz = 8.0f;
        float sx = (float)in.stick_x;
        float sy = (float)in.stick_y;
        if (sx * sx + sy * sy < dz * dz) {
            sx = 0.0f;
            sy = 0.0f;
        }
        const float inv = 1.0f / (float)JOYPAD_RANGE_N64_STICK_MAX;
        sx *= inv;
        sy *= inv;
        if (sx >  1.0f) sx =  1.0f;
        if (sx < -1.0f) sx = -1.0f;
        if (sy >  1.0f) sy =  1.0f;
        if (sy < -1.0f) sy = -1.0f;

        g_input[p].stick_x  = sx;
        g_input[p].stick_y  = sy;
        g_input[p].cstick_x = (float)in.cstick_x * inv;
        g_input[p].cstick_y = (float)in.cstick_y * inv;

        uint32_t held = (uint32_t)b.raw;
        g_input[p].buttons  = held;
        g_input[p].edges    = held & ~g_last_buttons[p];
        g_input[p].released = g_last_buttons[p] & ~held;
        g_last_buttons[p]   = held;
    }
}

const KilnInput *kiln_input_get(int port)
{
    if (port < 1 || port > JOYPAD_PORT_COUNT) port = 1;
    return &g_input[port - 1];
}

int kiln_input_held(int port, uint32_t mask)
{
    return (kiln_input_get(port)->buttons & mask) != 0;
}

int kiln_input_pressed(int port, uint32_t mask)
{
    return (kiln_input_get(port)->edges & mask) != 0;
}

int kiln_input_released(int port, uint32_t mask)
{
    return (kiln_input_get(port)->released & mask) != 0;
}