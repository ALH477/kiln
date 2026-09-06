/* SPDX-License-Identifier: MIT
 *
 * forge_map.h — emit one AABB brush as Quake .map text.
 *
 * Split out of forge_io.c so it can be compiled NATIVELY and asserted on: the
 * winding it writes is the fourth copy of a table whose sign decides whether a
 * brush survives quake_map.py's CSG at all, and it was the only one of the four
 * with no gate on it. forge_io.c cannot compile on the host — it is libdragon,
 * kiln_store and the voxel world — so the text emitter had to leave it.
 *
 * Same split, for the same reason, that kiln_voxmesh's vertex packing got out
 * of the mesher so nix/checks/kiln-logic could reach it.
 *
 * Includes <stdio.h>, <string.h> and the generated vocabulary. Nothing else.
 */
#ifndef FORGE_MAP_H
#define FORGE_MAP_H

#include <stddef.h>

/** Write one brush's six faces (a `{ ... }` block) into `out`. Returns the
 *  number of bytes written, in snprintf's sense. */
int forge_map_emit_box(char *out, size_t cap, const int mins[3],
                       const int maxs[3], const char *tex);

#endif /* FORGE_MAP_H */
