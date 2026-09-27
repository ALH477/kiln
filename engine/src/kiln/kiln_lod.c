/* SPDX-License-Identifier: MIT
 *
 * kiln_lod.c — LOD selector implementation.
 */
#include "kiln_lod.h"
#include "kiln_tile.h"

void fig_lod_init_defaults(FigLODConfig *cfg, float tile_size)
{
    /* LOD 0: within ~1.5 tiles (square of 1.5 * tile_size). */
    cfg->thresholds_sq[0] = (1.5f * tile_size) * (1.5f * tile_size);
    /* LOD 1: within ~4 tiles. */
    cfg->thresholds_sq[1] = (4.0f * tile_size) * (4.0f * tile_size);
    /* LOD 2: within ~9 tiles (max draw distance). */
    cfg->thresholds_sq[2] = (9.0f * tile_size) * (9.0f * tile_size);
    cfg->threshold_count = 3;
}

uint8_t fig_lod_select(const FigLODConfig *cfg, float dist_sq)
{
    for (uint8_t i = 0; i < cfg->threshold_count; i++) {
        if (dist_sq <= cfg->thresholds_sq[i])
            return i;
    }
    return FIG_TILE_MAX_LOD;  /* beyond max draw distance */
}

uint8_t fig_lod_selector_cb(int16_t tx, int16_t ty, float dist_sq,
                             void *user_ctx)
{
    (void)tx; (void)ty;
    return fig_lod_select((const FigLODConfig *)user_ctx, dist_sq);
}