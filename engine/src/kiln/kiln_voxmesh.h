/* SPDX-License-Identifier: MIT
 *
 * kiln_voxmesh.h — kiln_voxel's merged quads, packed into Tiny3D vertices.
 *
 * The other half of the seam kiln_voxel.h describes. Everything that needs a
 * T3DVertPacked, a texture or the RDP is here; everything that is arithmetic
 * over a block grid is there and is asserted on natively. This file cannot be
 * host-tested (it would mean asserting against a stub of Tiny3D) which is
 * exactly why so little logic lives in it: it packs, it batches, it draws.
 *
 * ── The batching, and why it is not kiln_map_draw's shape ───────────────
 *
 * kiln_map_draw does `t3d_vert_load(face->verts, 0, 8)` plus two t3d_tri_draw per
 * face — one RSP DMA for every quad. The RSP's vertex cache holds 70 vertices
 * (t3d.h's t3d_vert_load bounds `offset` 0..68 and `count` 1..70), so a mesh
 * that loads 8 at a time is using an eighth of it and paying eight times the
 * DMA setup. At kiln_map's own 1536-face ceiling that is 1536 loads a frame.
 *
 * Here the vertices for many quads are packed contiguously and loaded 68 at a
 * time (68 and not 70 because a quad is 4 vertices and 68 is the largest
 * multiple of 4 that fits, so no quad ever straddles two loads — a quad split
 * across a load boundary would index vertices that are no longer resident).
 * That is ~17 quads per DMA instead of one.
 *
 * ── Vertices are absolute world coordinates ────────────────────────────
 *
 * No per-chunk matrix. T3DVertPacked's position is int16, so the world is
 * bounded at +-32767 units either way — with 32-unit blocks that is +-1024
 * blocks, far outside kiln_voxel's 256x64x256 grid, so a matrix would buy
 * nothing but a push/pop per chunk and a second place for the offset to be
 * wrong. kiln_map.c does the same thing for the same reason.
 *
 * ── Shading ────────────────────────────────────────────────────────────
 *
 * Per-vertex RGBA carries a fixed per-face-direction brightness, so a cube
 * reads as a cube with the lights off. This is the same decision a generator
 * that bakes its own fixture rig into vertex colours makes (see the
 * n64-modeling skill), and it matters more here: an editor has no art
 * director, so if the six faces of a block shade identically the geometry is
 * unreadable and every judgement made standing in it is worthless. Lighting
 * is left to the game that consumes the exported .map.
 */
#ifndef KILN_VOXMESH_H
#define KILN_VOXMESH_H

#include <stdint.h>
#include <t3d/t3d.h>

#include "kiln_voxel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Vertices are loaded to the RSP 68 at a time: 4 per quad, and 68 is the
 * largest multiple of 4 within t3d_vert_load's 70-vertex cache. */
#define KILN_VOXMESH_BATCH 68

/* One chunk's packed geometry. `verts` points into a caller-owned arena — this
 * module allocates nothing, same contract as kiln_asset's arena and for the same
 * reason: a heap failure partway through a remesh is not recoverable on this
 * console, so the budget is decided at boot. */
typedef struct {
    T3DVertPacked *verts;      /* 2 vertices per struct; quads * 2 entries   */
    uint32_t       quad_count;
    uint32_t       vert_count; /* quad_count * 4                             */
} KilnVoxMesh;

/* A caller-owned uncached arena, sized once at boot. `kiln_voxmesh_arena_init`
 * takes memory the caller got from malloc_uncached (the vertices are DMA'd by
 * the RSP, so they must not sit behind a dirty cache line) and hands out
 * suballocations that are reset wholesale, not freed individually — the same
 * bump-allocator shape as kiln_scratch, because a remesh replaces every chunk's
 * geometry rather than editing it in place. */
typedef struct {
    T3DVertPacked *base;
    uint32_t       capacity;   /* entries, i.e. vertices / 2 */
    uint32_t       used;
} KilnVoxMeshArena;

void kiln_voxmesh_arena_init(KilnVoxMeshArena *a, T3DVertPacked *base,
                            uint32_t entries);
void kiln_voxmesh_arena_reset(KilnVoxMeshArena *a);
uint32_t kiln_voxmesh_arena_used_pct(const KilnVoxMeshArena *a);

/* Pack one chunk's quads into the arena. Returns 0, or -1 when the arena is
 * exhausted — in which case `out` is zeroed and the caller is expected to
 * SHOW that (Forge turns the mesh gauge red). A silently short mesh is a hole
 * in a wall, and this repo's whole verification story is built on the principle
 * that a missing thing has to be reported by something, because it is
 * indistinguishable from a camera pointed elsewhere.
 *
 * `atlas_tiles` is how many tiles across the CI4 atlas is (4 for the standard
 * 64x64 / 16x16 layout); block type N takes tile N-1, so type 0 (air) never
 * needs one.
 */
int kiln_voxmesh_build(const KilnVoxelWorld *w, int slot,
                      const KilnVoxelQuad *quads, uint32_t quad_count,
                      KilnVoxMeshArena *arena, int atlas_tiles,
                      KilnVoxMesh *out);

/* Draw one packed chunk. Sets NO render state: the caller has already chosen
 * the combiner, uploaded the atlas and set the draw flags, exactly as
 * kiln_map_draw inherits its state from kiln_scene_begin. Drawing several chunks
 * therefore costs one state setup, not one per chunk. */
void kiln_voxmesh_draw(const KilnVoxMesh *m);

/* ── The runtime CI4 atlas ─────────────────────────────────────────────
 *
 * 16 tiles of 16x16 in a 64x64 CI4 surface: 2 KB against a 4 KB TMEM, where the
 * same image as RGBA16 would be 8 KB and could not be loaded at all. The
 * 16-entry palette limit is also why kiln_voxel caps block types at 15.
 *
 * CI4 is not a size optimisation here, it is the format a palette-swap
 * mechanic is built on (see the n64-modeling skill's "CI4 and a
 * palette-swap contract"): swap the TLUT and every material changes colour
 * for 32 bytes of DMA and zero extra shaded pixels. So a Forge level is
 * automatically capable of that kind of swap, for any game that binds one to
 * a CI4-textured mesh with UVs.
 *
 * Two TLUTs are held, cold and veiled, and `kiln_voxatlas_bind` picks one. The
 * indices are shared, which is the contract mkVeilTexture exists to enforce
 * offline: two independent quantisations of the same image address different
 * colours, and the result renders in the wrong palette with nothing failing.
 */
#define KILN_VOXATLAS_TILE     16   /* pixels per tile side           */
#define KILN_VOXATLAS_ACROSS   4    /* tiles per row                  */
#define KILN_VOXATLAS_SIDE     (KILN_VOXATLAS_TILE * KILN_VOXATLAS_ACROSS)  /* 64 */
#define KILN_VOXATLAS_TILES    (KILN_VOXATLAS_ACROSS * KILN_VOXATLAS_ACROSS) /* 16 */
#define KILN_VOXATLAS_COLOURS  16

typedef enum { KILN_VOXATLAS_COLD = 0, KILN_VOXATLAS_VEILED = 1 } KilnVoxAtlasState;

typedef struct {
    /* One byte per pixel while editing, packed to 4bpp on upload. Painting a
     * nibble-packed buffer means every plot is a read-modify-write and every
     * bug is an off-by-one nibble; 4 KB of RDRAM is a much better trade than
     * that, and the packed copy is regenerated only when the atlas changes. */
    uint8_t   index[KILN_VOXATLAS_SIDE * KILN_VOXATLAS_SIDE];
    uint16_t  tlut[2][KILN_VOXATLAS_COLOURS];  /* RGBA5551, cold and veiled */
    surface_t surface;                        /* FMT_CI4, 64x64            */
    void     *packed;                         /* the 2 KB 4bpp buffer      */
    uint8_t   dirty;
} KilnVoxAtlas;

/* Allocates the packed buffer uncached (it is DMA'd to TMEM). Fills a default
 * 16-colour ramp and a placeholder tile per type so a fresh editor is not
 * looking at black. */
int  kiln_voxatlas_init(KilnVoxAtlas *a);
void kiln_voxatlas_close(KilnVoxAtlas *a);

void kiln_voxatlas_plot(KilnVoxAtlas *a, int tile, int px, int py, uint8_t colour);
uint8_t kiln_voxatlas_peek(const KilnVoxAtlas *a, int tile, int px, int py);
void kiln_voxatlas_fill_tile(KilnVoxAtlas *a, int tile, uint8_t colour);

/* Repack and upload. Call inside the 3D pass before drawing chunks. */
void kiln_voxatlas_bind(KilnVoxAtlas *a, KilnVoxAtlasState state);

#ifdef __cplusplus
}
#endif

#endif /* KILN_VOXMESH_H */
