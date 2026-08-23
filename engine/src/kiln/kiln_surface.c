/* SPDX-License-Identifier: MIT
 *
 * kiln_surface.c — see kiln_surface.h for the model.
 */

#include "kiln_surface.h"

#include "kiln_audio.h"

#include <libdragon.h>
#include <string.h>

/* The parameter type IS the bound. kiln_surface_register/get take a uint8_t,
 * which cannot exceed 255, and the table has 256 entries — so the runtime
 * `assertf(id < KILN_SURFACE_MAX)` both functions used to open with could
 * never fire. Compiling this module natively at -Werror surfaced it as a
 * -Wtype-limits warning that the cross build's -Wno-error had been swallowing
 * for as long as the module has existed.
 *
 * Asserting the invariant at COMPILE time instead keeps the guard and makes it
 * real: shrink KILN_SURFACE_MAX below 256 and this stops building, rather than
 * silently reopening an out-of-bounds index behind a check that reads like it
 * covers you. */
_Static_assert(KILN_SURFACE_MAX >= 256,
               "kiln_surface_get/register index by uint8_t, so the table needs "
               "at least 256 entries for every id to be in range");

static KilnSurfaceDef g_table[KILN_SURFACE_MAX];
static uint32_t g_warned_mask[KILN_SURFACE_MAX / 32];

void kiln_surface_register(uint8_t id, const KilnSurfaceDef *def)
{
    if (g_table[id].friction != 0.0f || g_table[id].footstep_sfx != 0)
        debugf("kiln_surface: overwriting surface %d\n", id);
    g_table[id] = *def;
}

const KilnSurfaceDef *kiln_surface_get(uint8_t id)
{
    if (g_table[id].friction == 0.0f && g_table[id].footstep_sfx == 0) {
        int bit = id % 32;
        int word = id / 32;
        if (!(g_warned_mask[word] & (1u << bit))) {
            g_warned_mask[word] |= (1u << bit);
            debugf("kiln_surface: unregistered id %d, returning zero default\n", id);
        }
    }
    return &g_table[id];
}

int kiln_surface_play_footstep(uint8_t id, float vol)
{
    const KilnSurfaceDef *d = kiln_surface_get(id);
    if (d->footstep_sfx < 0) return -1;
    return kiln_sfx_play_ex(d->footstep_sfx, -1, 1, vol, 0.5f);
}