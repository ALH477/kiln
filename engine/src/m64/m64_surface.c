/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_surface.c — see m64_surface.h for the model.
 */

#include "m64_surface.h"

#include "m64_audio.h"

#include <libdragon.h>
#include <string.h>

static M64SurfaceDef g_table[M64_SURFACE_MAX];
static uint32_t g_warned_mask[M64_SURFACE_MAX / 32];

void m64_surface_register(uint8_t id, const M64SurfaceDef *def)
{
    if (g_table[id].friction != 0.0f || g_table[id].footstep_sfx != 0)
        debugf("m64_surface: overwriting surface %d\n", id);
    g_table[id] = *def;
}

const M64SurfaceDef *m64_surface_get(uint8_t id)
{
    if (g_table[id].friction == 0.0f && g_table[id].footstep_sfx == 0) {
        int bit = id % 32;
        int word = id / 32;
        if (!(g_warned_mask[word] & (1u << bit))) {
            g_warned_mask[word] |= (1u << bit);
            debugf("m64_surface: unregistered id %d, returning zero default\n", id);
        }
    }
    return &g_table[id];
}

int m64_surface_play_footstep(uint8_t id, float vol)
{
    const M64SurfaceDef *d = m64_surface_get(id);
    if (d->footstep_sfx < 0) return -1;
    return m64_sfx_play_ex(d->footstep_sfx, -1, 1, vol, 0.5f);
}