/* SPDX-License-Identifier: MIT
 *
 * kiln_lod.h — LOD (Level of Detail) selector for tile-based rendering.
 *
 * ── Why LOD ────────────────────────────────────────────────────────────
 * On a 4 MB RDRAM console, drawing every tile at full detail is wasteful:
 * a tile 500 units away covers 2% of the screen but costs the same fill
 * rate as one at 50 units. LOD selects a cheaper representation for
 * distant tiles — fewer triangles, smaller textures, or a single merged
 * mesh covering a larger area. The residency manager (kiln_tile.h) asks
 * the LOD selector which level to load for each tile; the selector uses
 * distance + camera parameters to decide.
 *
 * ── Hierarchical LOD ───────────────────────────────────────────────────
 * The simplest LOD scheme is per-tile: each tile has lod0, lod1, lod2
 * versions, and the selector picks one based on distance. Hierarchical
 * LOD goes further: at lod2, one low-detail tile covers a 2×2 block of
 * lod1 tiles, and at lod3 one tile covers a 4×4 block. This is the
 * "clipmap" idea — coarser levels cover exponentially more area with
 * the same triangle count. The selector returns a LOD level and the
 * residency manager knows to load only that level (skipping children
 * when a parent is selected).
 *
 * ── Distance thresholds ───────────────────────────────────────────────
 * The thresholds are caller-configurable, not hardcoded. A game with a
 * 60-degree FOV needs different thresholds than one with a 90-degree
 * FOV. The caller passes an array of squared distance thresholds; the
 * selector does a single comparison per level (no sqrt in the hot path).
 *
 * ── Camera-relative precision ─────────────────────────────────────────
 * On large worlds, absolute world coordinates exceed s16.16 precision
 * (a 1000-unit world has 0.015-unit resolution). The selector computes
 * distances camera-relative: `dx = tile_center - camera_pos`, which
 * keeps the magnitudes small even for distant tiles. This is the same
 * precision trick Junkrunner64 uses in its render_batch_relative_mtx.
 *
 * Inspired by lambertjamesd/n64brew2025's LOD system (lod_scale-based
 * selection with 2D frustum culling) but designed from first principles:
 * distance thresholds instead of priority + lod_scale, hierarchical
 * support, and camera-relative computation.
 */
#ifndef KILN_LOD_H
#define KILN_LOD_H

#include <t3d/t3dmath.h>
#include <stdint.h>
#include "kiln_tile.h"  /* KILN_TILE_MAX_LOD */

#ifdef __cplusplus
extern "C" {
#endif

/** LOD configuration. */
typedef struct {
    /** Squared distance thresholds. thresholds[0] is the max distance
     *  for LOD 0, thresholds[1] for LOD 1, etc. A tile whose squared
     *  distance to the camera exceeds thresholds[i] is promoted to
     *  LOD i+1. The last threshold is the max draw distance — beyond
     *  it, the tile is not loaded at all (the residency manager
     *  handles this by excluding it from the desired set). */
    float thresholds_sq[KILN_TILE_MAX_LOD];
    /** Number of valid thresholds (determines the number of LOD levels). */
    uint8_t threshold_count;
} KilnLODConfig;

/** Initialise with sensible defaults for a 256-unit tile size:
 *  LOD 0 within 1 tile, LOD 1 within 4 tiles, LOD 2 within 9 tiles. */
void kiln_lod_init_defaults(KilnLODConfig *cfg, float tile_size);

/** Select LOD for a tile given its squared distance to the camera.
 *  Returns the LOD level (0 = highest detail), or KILN_TILE_MAX_LOD
 *  if the tile is beyond the max draw distance (should not be loaded). */
uint8_t kiln_lod_select(const KilnLODConfig *cfg, float dist_sq);

/** A callback wrapper for use with kiln_tile_update. Returns the LOD
 *  level for a tile given its squared distance. `user_ctx` must point
 *  to a KilnLODConfig. */
uint8_t kiln_lod_selector_cb(int16_t tx, int16_t ty, float dist_sq,
                             void *user_ctx);

/** Whether a tile at the given squared distance should be drawn at all
 *  (vs. being beyond the far threshold and skipped). */
static inline int kiln_lod_visible(const KilnLODConfig *cfg, float dist_sq) {
    if (cfg->threshold_count == 0) return 1;
    return dist_sq <= cfg->thresholds_sq[cfg->threshold_count - 1];
}

#ifdef __cplusplus
}
#endif

#endif /* KILN_LOD_H */