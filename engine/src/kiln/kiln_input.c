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