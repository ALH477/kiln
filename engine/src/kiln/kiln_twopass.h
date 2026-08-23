/* SPDX-License-Identifier: MIT
 *
 * kiln_twopass.h — two-pass render coordinator for LOD-based rendering.
 *
 * ── Why two passes ─────────────────────────────────────────────────────
 * The N64's Z-buffer is 16-bit and the RDP fill rate is limited. Drawing
 * distant low-detail geometry in the same pass as near high-detail
 * geometry wastes Z precision and fill on tiles that will be occluded.
 * A two-pass strategy splits rendering:
 *
 *   Pass 1 (far):  Draw LOD 2+ tiles with Z-buffer DISABLED. These are
 *                  distant enough that depth sorting doesn't matter —
 *                  they're background. No Z writes means no Z-fighting
 *                  with near geometry, and no per-pixel depth compare
 *                  cost. This is the same trick Junkrunner64 uses in
 *                  overworld_render_lod_1: rdpq_mode_zbuf(false, false)
 *                  for the distant LOD, then re-enabling it for the
 *                  near pass.
 *
 *   Pass 2 (near): Draw LOD 0–1 tiles with Z-buffer ENABLED. These are
 *                  close enough to need correct depth sorting, and they
 *                  write Z so that actors and effects drawn after them
 *                  are correctly occluded.
 *
 * ── Camera-relative transforms ─────────────────────────────────────────
 * On large worlds, absolute positions exceed s16.16 range. The coordinator
 * builds camera-relative transforms: each tile's world position is offset
 * by the camera position, keeping the values in a small range around
 * zero. This is the same precision trick as render_batch_relative_mtx in
 * Junkrunner64 and kiln_engine.h's KilnTransform.
 *
 * ── Integration with the existing engine ───────────────────────────────
 * The coordinator is a thin orchestration layer that sits between
 * kiln_scene_begin / kiln_gui_begin. It calls the user's draw callback twice:
 * once for the far pass (with Z disabled) and once for the near pass
 * (with Z enabled). The user's callback receives the pass number and
 * iterates the tile grid's loaded tiles at the appropriate LOD.
 *
 * Inspired by lambertjamesd/n64brew2025's overworld_render (which splits
 * LOD1 far rendering from LOD0 near rendering with different Z modes) but
 * designed from first principles: pass callbacks instead of hardcoded
 * step registration, camera-relative transforms via the scratch allocator,
 * and explicit Z-mode control per pass.
 */
#ifndef KILN_TWOPASS_H
#define KILN_TWOPASS_H

#include "kiln_scratch.h"
#include "kiln_tile.h"
#include "kiln_lod.h"
#include "kiln_engine.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Draw callback for a single pass. The user iterates the tile grid and
 *  draws tiles at the appropriate LOD level. `pass` is 0 (far) or 1 (near).
 *  `user_ctx` is passed through from kiln_twopass_render.
 *
 *  The callback is responsible for frustum culling — the coordinator
 *  does not test tiles against the camera frustum. A tile that is in
 *  the loaded window but behind the camera will still be passed to
 *  the callback; the callback should skip it (e.g. by checking the
 *  tile center against the scene's view-projection matrix). */
typedef void (*KilnPassDrawFn)(KilnTileGrid *grid, const KilnLODConfig *lod,
                              int pass, KilnScratch *scratch,
                              const KilnScene *scene, void *user_ctx);

/** Render two passes over the tile grid:
 *
 *  1. Far pass: disable Z-buffer, call draw_fn with pass=0.
 *     The far pass draws tiles at LOD >= far_lod_threshold (default 2).
 *
 *  2. Near pass: enable Z-buffer, call draw_fn with pass=1.
 *     The near pass draws tiles at LOD < far_lod_threshold.
 *
 *  Between passes, a pipe sync is issued to ensure the RDP has finished
 *  the far pass before the near pass changes Z mode.
 *
 *  The scratch allocator is used for per-pass transform matrices — the
 *  caller should call kiln_scratch_begin before this function and the
 *  transforms are valid until the next kiln_scratch_begin. */
void kiln_twopass_render(KilnTileGrid *grid, const KilnLODConfig *lod,
                        KilnScratch *scratch, const KilnScene *scene,
                        KilnPassDrawFn draw_fn, void *user_ctx);

/** Default LOD threshold: tiles at LOD >= this value go in the far pass. */
#define KILN_TWOPASS_FAR_LOD_THRESHOLD 2

#ifdef __cplusplus
}
#endif

#endif /* KILN_TWOPASS_H */