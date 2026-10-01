/* SPDX-License-Identifier: MIT
 *
 * dungeon_view -- what a chunk looks like, as rectangles.
 *
 * The part of the demo that has logic and no console: it includes only
 * <stdint.h>, so nix/checks/exsec-dungeon-parity.nix compiles and asserts on it
 * natively (the same split fig_voxel/fig_voxmesh make). main.c is glue over this
 * and libdragon.
 *
 * A chunk is 4,096 tiles, and drawing it as 4,096 fill rectangles would spend
 * the RDP on rock. The view reduces each ROW to runs of one tile kind, skipping
 * wall -- a generated chunk is about a third walkable, in a few hundred runs.
 */
#ifndef DUNGEON_VIEW_H
#define DUNGEON_VIEW_H

#include <stdint.h>

typedef struct {
    uint8_t x, y;   /* the run's first tile, in tiles              */
    uint8_t w;      /* its length in tiles, 1..64                  */
    uint8_t kind;   /* EXSEC_TILE_FLOOR / DOOR / TRAP, never wall  */
} DungeonRun;

/* A run is at least one non-wall tile, so 4,096 is a bound no chunk can exceed
 * and `dungeon_view_runs` has no overflow path to get wrong. */
#define DUNGEON_VIEW_MAX_RUNS 4096

/* Row-major runs of equal non-wall tiles in `chunk[0, 4096)`. Returns how many
 * were written to `runs`. */
int dungeon_view_runs(const uint8_t *chunk, DungeonRun *runs);

/* counts[kind] for kind 0..3. Tiles outside 0..3 are counted in none, so a
 * caller can tell by the sum. */
void dungeon_view_census(const uint8_t *chunk, uint32_t counts[4]);

#endif /* DUNGEON_VIEW_H */
