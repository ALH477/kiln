/* SPDX-License-Identifier: MIT
 *
 * kiln_twopass.c — two-pass render coordinator implementation.
 */
#include "kiln_twopass.h"
#include <libdragon.h>
#include <t3d/t3d.h>

void kiln_twopass_render(KilnTileGrid *grid, const KilnLODConfig *lod,
                        KilnScratch *scratch, const KilnScene *scene,
                        KilnPassDrawFn draw_fn, void *user_ctx)
{
    if (!draw_fn) return;

    /* Pass 1: far (Z disabled). */
    rdpq_sync_pipe();
    rdpq_mode_zbuf(false, false);
    draw_fn(grid, lod, 0, scratch, scene, user_ctx);

    /* Sync between passes: ensure the RDP has finished the far pass
     * before we change Z mode for the near pass. */
    rdpq_sync_pipe();
    rdpq_mode_zbuf(true, true);

    /* Pass 2: near (Z enabled). */
    draw_fn(grid, lod, 1, scratch, scene, user_ctx);

    rdpq_sync_pipe();
}