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
 * ── Brush CSG, and the AABB it produces ─────────────────────────────────
 * A Quake brush is a convex volume defined by the intersection of half-spaces,
 * each half-space described by three points ON its plane. Those three points
 * are NOT the face's corners — the file contains no vertices at all, and they
 * have to be derived by intersecting the planes, exactly as qbsp does when it
 * compiles a .map. kiln_map.c does that now (see its "Brush CSG" comment for
 * the algorithm and for what it is a port of):
 *   * every triple of planes meeting at a point that is inside every other
 *     plane is a vertex; grouped by plane and wound into a ring, that is the
 *     face — a real convex polygon of 3..MAX_FACE_VERTS (kiln_map.c's cap)
 *     vertices, drawn as a
 *     triangle fan
 *   * the AABB is the min/max of those true vertices, so an authored -64..64
 *     brush measures -64..64
 * It did neither of those until 2026-09: it read the three plane points as
 * three face corners and completed a parallelogram, so a 128-unit wall drew as
 * a 1x2-unit patch at one corner, and the AABB — min/max of the plane points —
 * came out a unit oversized because the second and third point of each plane
 * are conventionally one unit along the surface.
 *
 * A brush wound inside-out yields no vertices at all (the reversed half-spaces
 * intersect in nothing). That is the one case where the AABB still falls back
 * to the plane-point box, with a debugf, rather than handing a consumer a
 * brush with no collision; `./dev map-canon` repairs the file.
 *
 * Still NOT carried over: no texture or UV data is read, so a face's vertices
 * carry no texture coordinates and every face is drawn white.
 *
 * ── What was NOT carried over from id Tech 4 ────────────────────────────
 * No patches (bezier surfaces). No mesh primitives. No BSP / PVS / portals.
 * No texture/UV parsing (we skip the texture/UV/scale fields after the third
 * plane point). No map compile step — the "compiled" form is the AABB + face
 * polygon set produced here at load time. Real Doom 3 maps also need a CM model
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

/** One convex polygon, ready to be drawn as a triangle fan via `t3d_vert_load`
 *  + `t3d_tri_draw`. `verts` is an uncached buffer of `(vert_count + 1) / 2`
 *  T3DVertPacked structs — two vertices per struct, and an odd polygon's spare
 *  slot repeats the last vertex because the RSP's DMA moves whole pairs.
 *
 *  `vert_count` is 3..MAX_FACE_VERTS (kiln_map.c's own cap) and is NOT fixed: a brush face has as
 *  many vertices as the CSG gives it. Anything walking `verts` must read
 *  `vert_count` rather than assuming the 8 this struct used to always hold.
 *
 *  `rgba` is the face colour. The caller owns the buffer and frees it with
 *  `free_uncached`. */
typedef struct {
    T3DVertPacked *verts;
    uint32_t       rgba;
    uint8_t        vert_count;
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

/** Vertex colours for a parsed map's faces, which arrive flat white.
 *
 *  kiln_map parses no texture or UV data, so a level loaded as-is is a white
 *  box lit by its normals. Three examples had each grown their own shading
 *  pass over the packed vertices; this is that pass, once. It writes only
 *  colours — positions and normals are untouched — and costs one walk over
 *  the faces at load, nothing per frame.
 *
 *  A face is classified by its normal: up-facing at or below `floor_y` is
 *  floor (lerped `floor` -> `floor_edge` by XZ distance from the origin over
 *  `floor_radius`), up-facing above it is a raised top, down-facing is an
 *  underside, and everything else is a wall, lerped `wall_low` -> `wall_high`
 *  across the map's own height — dark at the foot, which reads as contact
 *  shadow. Walls facing ±Z are scaled by `z_face_shade` (1 = no change) so two
 *  walls meeting at a corner separate under flat light. Colours are RGBA8. */
typedef struct {
    uint32_t floor, floor_edge;
    float    floor_y, floor_radius;
    uint32_t top;
    uint32_t wall_low, wall_high;
    float    z_face_shade;
    uint32_t underside;
} KilnMapTint;

void kiln_map_tint(KilnMap *m, const KilnMapTint *t);

/** Draw every face in the map, one `t3d_vert_load` and one triangle fan per
 *  face. Intended to be called from a room's draw callback between
 *  kiln_scene_begin and kiln_actor_draw_all. */
void kiln_map_draw(const KilnMap *m);

#ifdef __cplusplus
}
#endif

#endif /* KILN_MAP_H */