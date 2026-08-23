/* SPDX-License-Identifier: MIT
 *
 * kiln_voxel.h — a block grid, and the two reductions that make it useful.
 *
 * The point of this module is not that blocks are fun to place. It is that a
 * run of blocks reduces to an **axis-aligned box**, and this engine already
 * speaks that language three times over:
 *
 *   - `KilnBrush` (kiln_clip.h) IS an AABB with a surface id. The collision world
 *     is a flat array of them.
 *   - `tools/mapmaker/src/mapio.js` emits Quake `.map` brushes that are AABBs
 *     and nothing else, and `kiln_map.c` reduces the six planes it parses right
 *     back down to `min`/`max`.
 *   - `kiln_room`'s streamed clip set is the same array, concatenated.
 *
 * So a voxel editor is not a new content path bolted onto the side. It is a
 * front-end for the content path that already exists, and the format conversion
 * is one algorithm — `kiln_voxel_boxes` — rather than an exporter.
 *
 * ── Two reductions, and why they are different ─────────────────────────
 *
 * `kiln_voxel_boxes()` is a **volume** decomposition: it partitions the solid
 * set into non-overlapping boxes that exactly tile it. That is what collision
 * and `.map` export want, because a brush is a solid.
 *
 * `kiln_voxel_quads()` is a **surface** extraction: greedy-merged rectangles over
 * the faces that have air on the other side. That is what rendering wants, and
 * it is emphatically not the same answer — drawing the boxes would draw the
 * faces where two boxes meet, and on this hardware fill rate is the limit, not
 * triangle count (the island terrain budget that held 60 fps is ~830 triangles;
 * a full-screen sea plane is the expensive part). Interior faces are the purest
 * form of wasted fill there is.
 *
 * ── Why the mesher is NOT in this module ───────────────────────────────
 *
 * `kiln_voxel_quads` stops at a list of rectangles in block coordinates. Turning
 * those into `T3DVertPacked` lives in kiln_voxmesh, because the moment a header
 * includes <t3d/t3d.h> it can no longer be compiled against
 * nix/checks/stub/ and asserted on natively — and this file is exactly the kind
 * of code that has to be. The greedy merge, the DDA raycast and the box
 * partition are pure arithmetic whose failure modes render perfectly and read
 * as level-design mistakes: a box that overlaps its neighbour is a collision
 * world the player sticks in, and a missing quad is a hole you can see through.
 * kiln_clip's broadphase shipped a bug of exactly that shape for months and
 * `nix build` never noticed. So the seam is deliberate: everything here goes in
 * nix/checks/kiln-logic-check.c, nothing here touches the RDP.
 *
 * ── Sizes, and the arithmetic behind them ──────────────────────────────
 *
 * A chunk is 16³ = 4096 blocks at one byte each. `KILN_VOXEL_MAX_CHUNKS` chunks
 * are resident, sparse — a chunk is allocated when a block in it is first set
 * and released when its last block is cleared, so an empty sky costs nothing.
 * At 24 chunks that is 96 KB of blocks plus a 2 KB index, ~100 KB total in one
 * caller-owned struct. Sized against the 4 MB this engine budgets for
 * everywhere (kiln_scratch.h, kiln_clip.h, kiln_map.h all say so), with no
 * Expansion Pak assumption — nothing in this engine checks for one.
 *
 * The addressable grid is 16x4x16 chunks = 256x64x256 blocks. At
 * `KILN_VOXEL_BLOCK_UNITS` = 32 that is 8192x2048x8192 world units, which is
 * comfortably inside the int16 that `T3DVertPacked.posA` and the `.map` format
 * both bottom out at (±32767). The *resident* limit is the binding one and it
 * is much lower: 24 non-empty chunks. That is reported, not enforced silently —
 * see `kiln_voxel_chunk_count`.
 *
 * 32 units per block is 0.5 m at the n64-modeling skill's 64-units-per-metre
 * convention. Half a metre is chosen so a 4-block doorway is 2 m and a
 * corridor reads at the right scale; it is a #define because a game with a
 * different world scale should change it once here rather than multiply at
 * every call site.
 *
 * ── What is deliberately NOT here ─────────────────────────────────────
 *
 * - **No block metadata / rotation / non-cube shapes.** A block is one byte of
 *   type. Slopes and stairs would break the "a run of blocks is a brush"
 *   identity that is the entire reason this module is shaped this way.
 * - **No lighting propagation.** Minecraft's flood-filled light levels are a
 *   per-block byte and a queue; this engine lights with at most 4 directional
 *   lights and no shadows, so per-block light would have nothing to feed. The
 *   mesher shades a face from its normal instead.
 * - **No infinite/streaming world.** The grid is fixed and small. `kiln_tile` +
 *   `kiln_lod` already exist for residency if a game ever wants that, and they
 *   are content-agnostic (`kiln_tile.h`'s slots hold an opaque `void *`).
 * - **No undo history.** That is the editor's business, not the world's, and it
 *   wants to record *actions* (a fill of 300 blocks is one undo step) rather
 *   than snapshots of a 100 KB struct.
 */
#ifndef KILN_VOXEL_H
#define KILN_VOXEL_H

#include <stdint.h>
#include <t3d/t3dmath.h>

#include "kiln_clip.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Dimensions ────────────────────────────────────────────────────────*/

#define KILN_VOXEL_CHUNK        16    /* blocks per chunk side              */
#define KILN_VOXEL_CHUNK_BLOCKS (KILN_VOXEL_CHUNK * KILN_VOXEL_CHUNK * KILN_VOXEL_CHUNK)

#define KILN_VOXEL_GRID_X       16    /* chunks                             */
#define KILN_VOXEL_GRID_Y       4
#define KILN_VOXEL_GRID_Z       16
#define KILN_VOXEL_GRID_SLOTS   (KILN_VOXEL_GRID_X * KILN_VOXEL_GRID_Y * KILN_VOXEL_GRID_Z)

#define KILN_VOXEL_DIM_X        (KILN_VOXEL_GRID_X * KILN_VOXEL_CHUNK)  /* 256 */
#define KILN_VOXEL_DIM_Y        (KILN_VOXEL_GRID_Y * KILN_VOXEL_CHUNK)  /*  64 */
#define KILN_VOXEL_DIM_Z        (KILN_VOXEL_GRID_Z * KILN_VOXEL_CHUNK)  /* 256 */

#ifndef KILN_VOXEL_MAX_CHUNKS
#define KILN_VOXEL_MAX_CHUNKS   24    /* resident, sparse: 24 * 4 KB = 96 KB */
#endif

#ifndef KILN_VOXEL_BLOCK_UNITS
#define KILN_VOXEL_BLOCK_UNITS  32    /* world units per block; 0.5 m at 64 u/m */
#endif

/* Block 0 is air. 1..15 are types, which is 15 and not 255 on purpose: a block
 * type indexes a tile in a 64x64 CI4 atlas, that atlas holds 16 tiles of 16x16,
 * and CI4 at 64x64 is 2 KB against a 4 KB TMEM where RGBA16 would be 8 KB and
 * could not load at all. The palette limit is the block limit; see
 * .claude/skills/n64-modeling/SKILL.md's CI4 section.
 */
#define KILN_VOXEL_AIR          0
#define KILN_VOXEL_TYPE_MAX     15

/* Face directions. The order is fixed and load-bearing: kiln_voxmesh indexes
 * tables by it, and so does the .map face emitter. */
typedef enum {
    KILN_VOXEL_XP = 0, KILN_VOXEL_XN,
    KILN_VOXEL_YP,     KILN_VOXEL_YN,
    KILN_VOXEL_ZP,     KILN_VOXEL_ZN,
    KILN_VOXEL_DIRS = 6,
} KilnVoxelDir;

/* ── The world ─────────────────────────────────────────────────────────*/

typedef struct {
    uint8_t  blocks[KILN_VOXEL_CHUNK_BLOCKS];
    uint8_t  cx, cy, cz;   /* chunk coordinates, so a slot knows where it is  */
    uint8_t  dirty;        /* set on every edit; the mesher clears it         */
    uint16_t solid;        /* non-air blocks; at 0 the slot is released       */
    uint16_t _pad;
} KilnVoxelChunk;

typedef struct {
    KilnVoxelChunk chunks[KILN_VOXEL_MAX_CHUNKS];
    /* slot + 1, so 0 means "no chunk here" and a zeroed world is a valid empty
     * one. Handle 0 meaning both "slot 0" and "invalid" is a bug this engine
     * has already shipped once, in kiln_cache; it cost the first asset acquired
     * from every fresh cache. */
    uint16_t index[KILN_VOXEL_GRID_SLOTS];
    uint8_t  used[KILN_VOXEL_MAX_CHUNKS];
    uint16_t chunk_count;
    /* World-unit offset added to block coordinates on export and on mesh. Lets
     * a level be centred on the origin without making block indices signed. */
    fm_vec3_t offset;
} KilnVoxelWorld;

/* A merged surface rectangle in block space. `dir` names the face; `w`/`h` are
 * extents in blocks along that face's two tangent axes (see kiln_voxel_dir_axes).
 * Eight bytes, because a busy chunk produces a lot of them and they are built
 * every time it is edited. */
typedef struct {
    int16_t x, y, z;   /* min corner block of the merged rectangle */
    uint8_t dir;       /* KilnVoxelDir                             */
    uint8_t block;     /* block type, i.e. which atlas tile       */
    uint8_t w, h;      /* extents in blocks, both >= 1            */
    uint8_t _pad[2];
} KilnVoxelQuad;

/* A raycast result. `px/py/pz` is the air block the ray entered through — i.e.
 * where a new block goes — and is what makes place-against-a-face work without
 * the caller re-deriving it from the normal and getting the sign wrong. */
typedef struct {
    int      hit;
    int      x, y, z;      /* the solid block that was hit          */
    int      px, py, pz;   /* the empty block in front of that face */
    int      nx, ny, nz;   /* face normal, exactly one is +-1       */
    uint8_t  dir;          /* KilnVoxelDir of the face that was hit  */
    uint8_t  block;        /* type of the block that was hit        */
    float    dist;         /* world units from the ray origin       */
} KilnVoxelHit;

/* ── World ops ─────────────────────────────────────────────────────────*/

void kiln_voxel_clear(KilnVoxelWorld *w);

int  kiln_voxel_in_bounds(int x, int y, int z);

/* Air outside the grid, so callers can probe a neighbour without bounds
 * checking first — which is what the mesher and the raycast both want. */
uint8_t kiln_voxel_get(const KilnVoxelWorld *w, int x, int y, int z);

/* 0 on success. KILN_VOXEL_EFULL when a new chunk was needed and all
 * KILN_VOXEL_MAX_CHUNKS slots are taken — reported, never silently dropped, and
 * the caller is expected to surface it (Forge turns the chunk counter red).
 * Setting air where there is already air, or a block outside the grid, is a
 * no-op returning 0: an edit that cannot happen is not an error. */
#define KILN_VOXEL_EFULL   (-1)
#define KILN_VOXEL_ETYPE   (-2)
int kiln_voxel_set(KilnVoxelWorld *w, int x, int y, int z, uint8_t block);

/* Inclusive box fill. Returns the number of blocks actually changed, or
 * KILN_VOXEL_EFULL if it ran out of chunk slots partway — in which case the
 * blocks it did change stay changed, because rolling back a fill would need a
 * copy of the world and the honest report is more useful than the illusion of
 * atomicity. */
int kiln_voxel_fill(KilnVoxelWorld *w, int x0, int y0, int z0,
                   int x1, int y1, int z1, uint8_t block);

int      kiln_voxel_chunk_count(const KilnVoxelWorld *w);
uint32_t kiln_voxel_solid_count(const KilnVoxelWorld *w);

/* Block-space AABB over every solid block, inclusive. Returns 0 and leaves the
 * outputs untouched when the world is empty. */
int kiln_voxel_bounds(const KilnVoxelWorld *w, int mins[3], int maxs[3]);

/* Chunk-slot iteration, for per-chunk meshing and dirty tracking. `slot` is an
 * index into `chunks`; unused slots are skipped by kiln_voxel_slot_next. */
int kiln_voxel_slot_first(const KilnVoxelWorld *w);
int kiln_voxel_slot_next(const KilnVoxelWorld *w, int slot);

/* ── Raycast ───────────────────────────────────────────────────────────
 *
 * Amanatides-Woo DDA over the grid: exact, no step size to tune, and it visits
 * every cell the ray actually passes through in order. `dir` need not be
 * normalised. Reports the first solid block within `max_dist` world units.
 *
 * This is the reticle, and it is also the reason the editor does not need
 * kiln_clip for picking: a grid lookup is O(cells crossed) with no brush list at
 * all, where kiln_clip would be O(brushes) against a set that changes on every
 * edit.
 */
int kiln_voxel_raycast(const KilnVoxelWorld *w, const fm_vec3_t *origin,
                      const fm_vec3_t *dir, float max_dist, KilnVoxelHit *out);

/* ── Reduction 1: volume -> boxes ──────────────────────────────────────
 *
 * Partitions the solid set into non-overlapping boxes that exactly tile it,
 * greedy along +X then +Z then +Y, splitting on block type so a box has one
 * surface. Deterministic: the same world always yields the same boxes in the
 * same order, which is what makes a `.map` export diffable and a round-trip
 * check possible at all.
 *
 * `surface_of` maps a block type to a KilnBrush surface id, or NULL for
 * "surface = block type", which is the identity a game usually wants
 * (kiln_surface.h's table is 256 entries and block types are 1..15).
 *
 * Returns the number of boxes written, or the NEGATIVE of the number that would
 * have been needed if `cap` was too small — so a caller can size a buffer in
 * one retry instead of guessing. Nothing is written in that case.
 */
int kiln_voxel_boxes(const KilnVoxelWorld *w, KilnBrush *out, uint16_t cap,
                    const uint8_t *surface_of);

/* ── Reduction 2: surface -> merged quads ──────────────────────────────
 *
 * Greedy-merged rectangles over exposed faces of ONE chunk, six directions,
 * merging first along the face's u axis and then along v. Only faces whose
 * neighbour is air are emitted — including neighbours in the adjacent chunk,
 * which is why this takes the world and not the chunk: meshing a chunk in
 * isolation welds a wall across every chunk seam, and that reads as the level
 * being made of boxes, which was in fact true.
 *
 * Returns quads written, or the negative of the count needed if `cap` is too
 * small (nothing written), same contract as kiln_voxel_boxes.
 */
int kiln_voxel_quads(const KilnVoxelWorld *w, int slot,
                    KilnVoxelQuad *out, uint32_t cap);

/* The two tangent axes of a face direction, and its normal. Exposed because
 * kiln_voxmesh and the .map emitter must agree with the mesher about which axis
 * `w` runs along, and three copies of that table would be three chances to
 * disagree. `axes[0]` = u (the `w` extent), `axes[1]` = v (the `h` extent),
 * `axes[2]` = the normal axis; `sign` is +1 or -1. */
void kiln_voxel_dir_axes(uint8_t dir, int axes[3], int *sign);

#ifdef __cplusplus
}
#endif

#endif /* KILN_VOXEL_H */
