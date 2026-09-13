/* SPDX-License-Identifier: MIT
 *
 * kiln_prim.h — flat-shaded primitives and a scene preset, for ROMs that have
 * no art yet and should not look like it.
 *
 * ── Why this exists ─────────────────────────────────────────────────────
 * Twelve examples each carried their own cube builder, and every one of them
 * was the same eight shared corners with a normal pointing out through each
 * corner. Eight vertices cannot carry six face normals, so the lighting was
 * smeared across every edge and a lit box read as a soft blob — the comment in
 * examples/engine said as much. The fix is 24 vertices, four per face, which
 * is exactly how nix/checks/kiln-scene-check.c builds its cube; this is that
 * construction, once, in the engine.
 *
 * ── The quad convention ────────────────────────────────────────────────
 * Every primitive is a list of quads, four vertices each, wound counter-
 * clockwise seen from the side the normal points to. That is kiln_voxmesh's
 * convention and its draw loop: KILN_PRIM_BATCH vertices per t3d_vert_load,
 * 68 and not 70 because a quad may not straddle a load. A floor of 256 cells
 * is 16 loads and no amount of cells can overrun the RSP's vertex cache.
 *
 * ── Integer positions ──────────────────────────────────────────────────
 * Positions are int16 world units, the integer part of the ucode's s16.16 —
 * the same as every hand-packed vertex in this engine. A box of half-extent
 * 0.3 rounds to nothing; build it at a size that survives and scale it down
 * with a KilnTransform, which is quantised to s16.16 and keeps the fraction.
 *
 * ── Per-face colour, not per-vertex shading ────────────────────────────
 * `top`, `side` and `bottom` are RGBA8 (0xRRGGBBAA). Three colours rather than
 * one because a top face a little lighter than its sides reads as solid even
 * under flat ambient light, which is what the eye uses to find the ground.
 */
#ifndef KILN_PRIM_H
#define KILN_PRIM_H

#include "kiln_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Vertices per t3d_vert_load. A multiple of 4 below the 70-entry cache. */
#define KILN_PRIM_BATCH 68

/** Largest floor, in cells per side. 32x32 is 1024 quads, 16 KB of vertices. */
#define KILN_PRIM_FLOOR_MAX_CELLS 32

typedef struct {
    T3DVertPacked *verts;      /**< uncached; two vertices per entry          */
    uint16_t       quad_count;
    uint16_t       vert_count; /**< always quad_count * 4                     */
} KilnPrim;

/** An axis-aligned box: 6 quads, 24 vertices, one true normal per face.
 *
 *  `offset` moves the box relative to the origin its transform rotates about.
 *  A door is the case: `offset = {half.x, 0, 0}` puts the hinge on the -X edge,
 *  so rotating the transform swings the door instead of spinning it in place.
 *
 *  Returns 0 on success, -1 on allocation failure (the prim is left empty). */
int kiln_prim_box(KilnPrim *out, fm_vec3_t offset, fm_vec3_t half,
                  uint32_t top, uint32_t side, uint32_t bottom);

/** A checkerboard on y = 0, facing +Y, spanning [-extent, +extent] on X and Z.
 *  `cells` per side, clamped to 1..KILN_PRIM_FLOOR_MAX_CELLS. `cells = 1` is a
 *  single quad, which is also the cheapest blob shadow there is.
 *
 *  Returns 0 on success, -1 on allocation failure. */
int kiln_prim_floor(KilnPrim *out, float extent, int cells,
                    uint32_t rgba_a, uint32_t rgba_b);

/** Draw with whatever transform and render state are current. Sets none. */
void kiln_prim_draw(const KilnPrim *p);

void kiln_prim_free(KilnPrim *p);

/** Light a scene so primitives read as objects in a place.
 *
 *  - clear colour = fog colour = `sky`, so geometry ends in haze and not at an
 *    edge (kiln_engine.h's fog comment explains why that matching matters);
 *  - a warm key from above and in front (light 0) and a cool rim from behind
 *    (lights[0]), `light_count = 2`, so no face sits at bare ambient. Both
 *    directions point toward the light (overhead = positive y);
 *  - ambient at a quarter of the sky, tinted by it.
 *
 *  Every field stays a plain KilnScene field: override any of them after. Pass
 *  `fog_far <= fog_near` to leave fog off. Call after kiln_scene_init. */
void kiln_prim_stage(KilnScene *s, color_t sky, float fog_near, float fog_far);

/** Pack an 8-bit-per-channel colour, for the rgba arguments above. */
static inline uint32_t kiln_prim_rgba(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFFu;
}

/** Scale a packed colour's RGB by `k` (0..1+), keeping alpha. For "this crate
 *  is asleep" and "this room is not the current one" without a second table. */
uint32_t kiln_prim_shade(uint32_t rgba, float k);

#ifdef __cplusplus
}
#endif

#endif /* KILN_PRIM_H */
