/* SPDX-License-Identifier: MIT
 *
 * kiln_map.h — a clean-room idMapFile analogue: load a Quake-format `.map`
 * and turn it into `KilnBrush` collision + `KilnRoomSpawn` templates with
 * `KilnDict` spawn args. See CLAUDE.md's Phase C notes for what was and
 * wasn't carried over from id Tech 4's map pipeline.
 *
 * ── Why Quake .map text format ──────────────────────────────────────────
 * `assets/quake_test.map` already exists, and TrenchBroom (the standard
 * Quake/Doom editor) emits it. The file is human-readable, version-controlled
 * friendly, and the parser is small enough to run on a 4 MB console at boot
 * once per room. Entity epairs map 1:1 to `KilnDict`; brushes reduce to AABBs
 * for collision and hand-rolled face quads for rendering. One `.map` = one
 * `KilnMap` = one room for the demo; multi-room games load several `.map`
 * files and connect them via entity `target_room` epairs later.
 *
 * ── Brush reduction to AABB ─────────────────────────────────────────────
 * A Quake brush is a convex volume defined by the intersection of half-spaces,
 * each half-space described by three plane points. Doom 3 builds a full
 * collision model from those planes; here we only need the AABB for collision
 * and a quad per face for rendering, so we reduce each brush to:
 *   * componentwise min/max of all its plane points → AABB
 *   * one parallelogram per face using the three plane points
 * Non-axis-aligned faces render as parallelograms, not true polygons. That
 * is acceptable for rectangular OoT-style rooms and is flagged as a known
 * limit in the header comment.
 *
 * ── What was NOT carried over from id Tech 4 ────────────────────────────
 * No patches (bezier surfaces). No mesh primitives. No BSP / PVS / portals.
 * No texture/UV parsing (we skip the texture/UV/scale fields after the third
 * plane point). No map compile step — the "compiled" form is the AABB + face
 * quad set produced here at load time. Real Doom 3 maps also need a CM model
 * built offline; our collision is the AABB approximation.
 */
#ifndef KILN_MAP_H
#define KILN_MAP_H

#include <stdint.h>
#include "kiln_clip.h"
#include "kiln_dict.h"
#include "kiln_room.h"
#include "kiln_actor.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One face ready to be drawn as two triangles via `t3d_vert_load` +
 *  `t3d_tri_draw`. `verts` is an uncached buffer of 4 T3DVertPacked structs
 *  (8 vertices total, two packed per struct); `rgba` is the face colour.
 *  The caller owns the buffer and frees it with `free_uncached`. */
typedef struct {
    T3DVertPacked *verts;
    uint32_t       rgba;
} KilnMapFace;

/** A parsed .map room. `brushes` and `faces` are module-owned arrays allocated
 *  during kiln_map_load; kiln_map_free releases them. `spawns` are the non-
 *  worldspawn entities translated into KilnRoomSpawn form, each carrying a
 *  populated `KilnDict`. `world_aabb` is the union of all brush AABBs. */
typedef struct {
    KilnBrush     *brushes;
    uint16_t      brush_count;

    KilnMapFace   *faces;
    uint16_t      face_count;

    KilnRoomSpawn *spawns;
    uint16_t      spawn_count;

    fm_vec3_t     world_aabb_min;
    fm_vec3_t     world_aabb_max;
} KilnMap;

/** Bind a Quake `classname` string to an actor profile id. Must be called
 *  before kiln_map_load for any classnames the map uses; unknown classnames
 *  are skipped (with a debugf) rather than failing the whole load. */
void kiln_map_register_classname(const char *classname, uint16_t profile_id);

/** Parse a `.map` from a DFS path. Returns 0 on success, -1 on any I/O or
 *  parse failure. On success the caller must call kiln_map_free. Brushes are
 *  tagged with surface ids: index 0 for worldspawn brushes, index 1 for any
 *  brush whose classname (via a yet-unloaded entity) is "func_metal" etc.
 *  For now all worldspawn brushes use surface 0; kiln_map's role is to lay the
 *  data out, not to own the surface table semantics. */
int  kiln_map_load(KilnMap *out, const char *dfs_path);
void kiln_map_free(KilnMap *m);

/** Draw every face in the map. Intended to be called from a room's draw
 *  callback between kiln_scene_begin and kiln_actor_draw_all. */
void kiln_map_draw(const KilnMap *m);

#ifdef __cplusplus
}
#endif

#endif /* KILN_MAP_H */