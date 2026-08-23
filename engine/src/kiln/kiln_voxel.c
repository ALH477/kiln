/* SPDX-License-Identifier: MIT
 *
 * kiln_voxel.c — see kiln_voxel.h for what this module is for and what it is
 * deliberately not.
 *
 * Everything here is integer or single-precision arithmetic over a caller-owned
 * struct, with no libdragon or Tiny3D call at all beyond fm_vec3_t's layout.
 * That is on purpose and it is the whole reason the mesher lives elsewhere: this
 * file compiles natively against nix/checks/stub/ and is asserted on in
 * nix/checks/kiln-logic-check.c, in milliseconds, where a wrong greedy merge is a
 * failed assertion instead of a hole in a wall someone eventually notices.
 */
#include <string.h>
#include <math.h>

#include "kiln_voxel.h"

/* ── Indexing ──────────────────────────────────────────────────────────*/

#define GI(cx, cy, cz) (((cz) * KILN_VOXEL_GRID_Y + (cy)) * KILN_VOXEL_GRID_X + (cx))
#define BI(x, y, z)    (((z) * KILN_VOXEL_CHUNK + (y)) * KILN_VOXEL_CHUNK + (x))

/* Face tables. ONE definition, exported through kiln_voxel_dir_axes, because the
 * mesher, the vertex packer and the .map emitter all have to agree about which
 * axis a quad's `w` runs along and three copies would be three chances to
 * disagree — the failure being a wall whose texture is rotated, or a brush that
 * is inside out. Axis order is u, v, normal. */
static const int DIR_AXES[KILN_VOXEL_DIRS][3] = {
    /* XP */ { 2, 1, 0 },   /* u = z, v = y, n = x */
    /* XN */ { 2, 1, 0 },
    /* YP */ { 0, 2, 1 },   /* u = x, v = z, n = y */
    /* YN */ { 0, 2, 1 },
    /* ZP */ { 0, 1, 2 },   /* u = x, v = y, n = z */
    /* ZN */ { 0, 1, 2 },
};
static const int DIR_SIGN[KILN_VOXEL_DIRS] = { +1, -1, +1, -1, +1, -1 };

void kiln_voxel_dir_axes(uint8_t dir, int axes[3], int *sign)
{
    if (dir >= KILN_VOXEL_DIRS) dir = 0;
    axes[0] = DIR_AXES[dir][0];
    axes[1] = DIR_AXES[dir][1];
    axes[2] = DIR_AXES[dir][2];
    if (sign) *sign = DIR_SIGN[dir];
}

int kiln_voxel_in_bounds(int x, int y, int z)
{
    return x >= 0 && y >= 0 && z >= 0 &&
           x < KILN_VOXEL_DIM_X && y < KILN_VOXEL_DIM_Y && z < KILN_VOXEL_DIM_Z;
}

void kiln_voxel_clear(KilnVoxelWorld *w)
{
    /* memset over the whole struct rather than a per-field reset: `index` is
     * slot+1 so all-zero IS the valid empty state, and a chunk's contents are
     * unreachable while its index entry is 0. That identity is the reason the
     * +1 is there at all (see the header's note about kiln_cache's handle 0). */
    memset(w, 0, sizeof *w);
}

static const KilnVoxelChunk *chunk_at(const KilnVoxelWorld *w, int cx, int cy, int cz)
{
    uint16_t e = w->index[GI(cx, cy, cz)];
    return e ? &w->chunks[e - 1] : NULL;
}

/* Bounds are checked HERE, before the index is addressed. GI() on a negative
 * chunk coordinate produces a negative subscript, and w->index[-1] is a read
 * off the front of the struct that happens to land in `chunks` — it would
 * "work" for years and corrupt a block. The caller passes neighbours that may
 * be off the grid by construction, so the guard belongs on this side. */
static void mark_dirty(KilnVoxelWorld *w, int cx, int cy, int cz)
{
    if (cx < 0 || cy < 0 || cz < 0) return;
    if (cx >= KILN_VOXEL_GRID_X || cy >= KILN_VOXEL_GRID_Y || cz >= KILN_VOXEL_GRID_Z)
        return;
    uint16_t e = w->index[GI(cx, cy, cz)];
    if (e) w->chunks[e - 1].dirty = 1;
}

uint8_t kiln_voxel_get(const KilnVoxelWorld *w, int x, int y, int z)
{
    if (!kiln_voxel_in_bounds(x, y, z)) return KILN_VOXEL_AIR;
    const KilnVoxelChunk *c = chunk_at(w, x / KILN_VOXEL_CHUNK,
                                        y / KILN_VOXEL_CHUNK,
                                        z / KILN_VOXEL_CHUNK);
    if (!c) return KILN_VOXEL_AIR;
    return c->blocks[BI(x % KILN_VOXEL_CHUNK, y % KILN_VOXEL_CHUNK,
                        z % KILN_VOXEL_CHUNK)];
}

int kiln_voxel_set(KilnVoxelWorld *w, int x, int y, int z, uint8_t block)
{
    if (!kiln_voxel_in_bounds(x, y, z)) return 0;
    if (block > KILN_VOXEL_TYPE_MAX) return KILN_VOXEL_ETYPE;

    int cx = x / KILN_VOXEL_CHUNK, cy = y / KILN_VOXEL_CHUNK, cz = z / KILN_VOXEL_CHUNK;
    int gi = GI(cx, cy, cz);
    uint16_t e = w->index[gi];

    if (!e) {
        /* Clearing air out of a chunk that does not exist is not an error and
         * must not allocate — otherwise a break aimed at the sky costs a chunk
         * slot, and the slot budget is the scarce thing here. */
        if (block == KILN_VOXEL_AIR) return 0;
        int slot = -1;
        for (int i = 0; i < KILN_VOXEL_MAX_CHUNKS; i++)
            if (!w->used[i]) { slot = i; break; }
        if (slot < 0) return KILN_VOXEL_EFULL;
        w->used[slot] = 1;
        KilnVoxelChunk *nc = &w->chunks[slot];
        memset(nc, 0, sizeof *nc);
        nc->cx = (uint8_t)cx; nc->cy = (uint8_t)cy; nc->cz = (uint8_t)cz;
        w->index[gi] = (uint16_t)(slot + 1);
        w->chunk_count++;
        e = (uint16_t)(slot + 1);
    }

    KilnVoxelChunk *c = &w->chunks[e - 1];
    int bi = BI(x % KILN_VOXEL_CHUNK, y % KILN_VOXEL_CHUNK, z % KILN_VOXEL_CHUNK);
    uint8_t old = c->blocks[bi];
    if (old == block) return 0;

    c->blocks[bi] = block;
    if (old == KILN_VOXEL_AIR) c->solid++;
    else if (block == KILN_VOXEL_AIR) c->solid--;
    c->dirty = 1;

    /* A neighbouring chunk's surface changes when a block on this side of the
     * seam appears or vanishes, so its mesh is stale too. Marking only the
     * edited chunk is how a voxel editor grows a visible grid of seams: the wall
     * is right, the mesh next door still has a face where air now is. Only the
     * faces on the chunk boundary can be affected, so only those neighbours are
     * touched. */
    int lx = x % KILN_VOXEL_CHUNK, ly = y % KILN_VOXEL_CHUNK, lz = z % KILN_VOXEL_CHUNK;
    if (lx == 0)                   mark_dirty(w, cx - 1, cy, cz);
    if (lx == KILN_VOXEL_CHUNK - 1) mark_dirty(w, cx + 1, cy, cz);
    if (ly == 0)                   mark_dirty(w, cx, cy - 1, cz);
    if (ly == KILN_VOXEL_CHUNK - 1) mark_dirty(w, cx, cy + 1, cz);
    if (lz == 0)                   mark_dirty(w, cx, cy, cz - 1);
    if (lz == KILN_VOXEL_CHUNK - 1) mark_dirty(w, cx, cy, cz + 1);

    /* Release an emptied chunk. Without this, hollowing out a room leaves its
     * chunks resident forever and the 24-slot budget is spent on air. */
    if (c->solid == 0) {
        w->used[e - 1] = 0;
        w->index[gi] = 0;
        w->chunk_count--;
    }
    return 0;
}

int kiln_voxel_fill(KilnVoxelWorld *w, int x0, int y0, int z0,
                   int x1, int y1, int z1, uint8_t block)
{
    if (block > KILN_VOXEL_TYPE_MAX) return KILN_VOXEL_ETYPE;
    /* Accept the corners in any order — a drag selection has no reason to run
     * in +X+Y+Z, and a caller that has to sort them first will eventually
     * forget to. */
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (z0 > z1) { int t = z0; z0 = z1; z1 = t; }

    int changed = 0;
    for (int z = z0; z <= z1; z++)
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                if (!kiln_voxel_in_bounds(x, y, z)) continue;
                if (kiln_voxel_get(w, x, y, z) == block) continue;
                int st = kiln_voxel_set(w, x, y, z, block);
                if (st == KILN_VOXEL_EFULL) return KILN_VOXEL_EFULL;
                changed++;
            }
    return changed;
}

int kiln_voxel_chunk_count(const KilnVoxelWorld *w) { return w->chunk_count; }

uint32_t kiln_voxel_solid_count(const KilnVoxelWorld *w)
{
    uint32_t n = 0;
    for (int i = 0; i < KILN_VOXEL_MAX_CHUNKS; i++)
        if (w->used[i]) n += w->chunks[i].solid;
    return n;
}

int kiln_voxel_slot_first(const KilnVoxelWorld *w)
{
    for (int i = 0; i < KILN_VOXEL_MAX_CHUNKS; i++) if (w->used[i]) return i;
    return -1;
}

int kiln_voxel_slot_next(const KilnVoxelWorld *w, int slot)
{
    for (int i = slot + 1; i < KILN_VOXEL_MAX_CHUNKS; i++) if (w->used[i]) return i;
    return -1;
}

int kiln_voxel_bounds(const KilnVoxelWorld *w, int mins[3], int maxs[3])
{
    int lo[3] = { KILN_VOXEL_DIM_X, KILN_VOXEL_DIM_Y, KILN_VOXEL_DIM_Z };
    int hi[3] = { -1, -1, -1 };
    int any = 0;

    for (int s = kiln_voxel_slot_first(w); s >= 0; s = kiln_voxel_slot_next(w, s)) {
        const KilnVoxelChunk *c = &w->chunks[s];
        int ox = c->cx * KILN_VOXEL_CHUNK;
        int oy = c->cy * KILN_VOXEL_CHUNK;
        int oz = c->cz * KILN_VOXEL_CHUNK;
        for (int z = 0; z < KILN_VOXEL_CHUNK; z++)
            for (int y = 0; y < KILN_VOXEL_CHUNK; y++)
                for (int x = 0; x < KILN_VOXEL_CHUNK; x++) {
                    if (c->blocks[BI(x, y, z)] == KILN_VOXEL_AIR) continue;
                    int p[3] = { ox + x, oy + y, oz + z };
                    for (int a = 0; a < 3; a++) {
                        if (p[a] < lo[a]) lo[a] = p[a];
                        if (p[a] > hi[a]) hi[a] = p[a];
                    }
                    any = 1;
                }
    }
    if (!any) return 0;
    if (mins) { mins[0] = lo[0]; mins[1] = lo[1]; mins[2] = lo[2]; }
    if (maxs) { maxs[0] = hi[0]; maxs[1] = hi[1]; maxs[2] = hi[2]; }
    return 1;
}

/* ── Raycast: Amanatides-Woo DDA ───────────────────────────────────────*/

int kiln_voxel_raycast(const KilnVoxelWorld *w, const fm_vec3_t *origin,
                      const fm_vec3_t *dir, float max_dist, KilnVoxelHit *out)
{
    if (out) memset(out, 0, sizeof *out);

    const float B = (float)KILN_VOXEL_BLOCK_UNITS;
    /* Into block space, so the DDA works in unit cells and the only place the
     * block size appears is here and at the end. */
    float o[3], d[3];
    for (int a = 0; a < 3; a++) {
        o[a] = (origin->v[a] - w->offset.v[a]) / B;
        d[a] = dir->v[a];
    }
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1e-6f) return 0;
    for (int a = 0; a < 3; a++) d[a] /= len;

    int   cell[3] = { (int)floorf(o[0]), (int)floorf(o[1]), (int)floorf(o[2]) };
    int   step[3];
    float tmax[3], tdelta[3];

    for (int a = 0; a < 3; a++) {
        if (d[a] > 1e-6f) {
            step[a]   = 1;
            tdelta[a] = 1.0f / d[a];
            tmax[a]   = ((float)(cell[a] + 1) - o[a]) / d[a];
        } else if (d[a] < -1e-6f) {
            step[a]   = -1;
            tdelta[a] = -1.0f / d[a];
            tmax[a]   = ((float)cell[a] - o[a]) / d[a];
        } else {
            /* Exactly axis-parallel: never crosses a boundary on this axis.
             * INFINITY rather than a large float so no accumulation of tdelta
             * can ever make it the smallest of the three. */
            step[a]   = 0;
            tdelta[a] = INFINITY;
            tmax[a]   = INFINITY;
        }
    }

    /* Standing inside a solid block reports it immediately, with the face
     * behind the ray. A caller that wants "the first block I am NOT in" can
     * check hit.dist == 0; silently skipping it would make a block placed on
     * top of the camera unbreakable. */
    if (kiln_voxel_get(w, cell[0], cell[1], cell[2]) != KILN_VOXEL_AIR) {
        if (out) {
            out->hit = 1;
            out->x = cell[0]; out->y = cell[1]; out->z = cell[2];
            out->px = cell[0]; out->py = cell[1]; out->pz = cell[2];
            out->block = kiln_voxel_get(w, cell[0], cell[1], cell[2]);
            out->dist = 0.0f;
            out->dir = KILN_VOXEL_YP;
            out->ny = 1;
        }
        return 1;
    }

    float max_cells = max_dist / B;
    int   axis = -1;

    for (int guard = 0; guard < 4096; guard++) {
        /* Advance along whichever axis has the nearest boundary. */
        axis = 0;
        if (tmax[1] < tmax[axis]) axis = 1;
        if (tmax[2] < tmax[axis]) axis = 2;
        if (tmax[axis] > max_cells) return 0;

        cell[axis] += step[axis];
        float t = tmax[axis];
        tmax[axis] += tdelta[axis];

        /* Give up only when the ray can no longer RE-ENTER the grid on this
         * axis: past the far edge while moving positive, or past zero while
         * moving negative. Anything else is still approaching.
         *
         * The first version bailed on any out-of-bounds cell, which sounds
         * equivalent and is not: an editor camera sits OUTSIDE the level looking
         * in, so the very first step was already at a negative cell and every
         * ray missed. The whole reticle read "aim -" with a room filling the
         * screen — a failure that looks like the camera being wrong, or the
         * world being empty, and is neither. */
        static const int DIM[3] = { KILN_VOXEL_DIM_X, KILN_VOXEL_DIM_Y, KILN_VOXEL_DIM_Z };
        if ((step[axis] > 0 && cell[axis] >= DIM[axis]) ||
            (step[axis] < 0 && cell[axis] < 0))
            return 0;

        uint8_t b = kiln_voxel_get(w, cell[0], cell[1], cell[2]);
        if (b == KILN_VOXEL_AIR) continue;

        if (out) {
            out->hit = 1;
            out->x = cell[0]; out->y = cell[1]; out->z = cell[2];
            out->block = b;
            out->dist = t * B;
            /* The place-here cell is the one we came from, which is exactly the
             * cell minus the step we just took. Deriving it this way rather than
             * from the normal is what makes it impossible to get the sign
             * backwards — the classic "blocks appear inside the wall" bug. */
            out->px = cell[0]; out->py = cell[1]; out->pz = cell[2];
            if (axis == 0) { out->px -= step[0]; out->nx = -step[0]; out->dir = step[0] > 0 ? KILN_VOXEL_XN : KILN_VOXEL_XP; }
            if (axis == 1) { out->py -= step[1]; out->ny = -step[1]; out->dir = step[1] > 0 ? KILN_VOXEL_YN : KILN_VOXEL_YP; }
            if (axis == 2) { out->pz -= step[2]; out->nz = -step[2]; out->dir = step[2] > 0 ? KILN_VOXEL_ZN : KILN_VOXEL_ZP; }
        }
        return 1;
    }
    return 0;
}

/* ── Reduction 1: volume -> boxes ──────────────────────────────────────
 *
 * A 3D greedy partition. Scan in y, z, x order; at the first unclaimed solid
 * block, grow a run along +X while the type matches and nothing is claimed,
 * then grow that run along +Z as a slab, then grow the slab along +Y. Claim
 * everything in the resulting box and emit it.
 *
 * Two properties are load-bearing and both are asserted in the host check:
 *   - the boxes are DISJOINT and their union is exactly the solid set, because
 *     they become collision brushes and a double-covered block is a brush the
 *     player sticks inside;
 *   - the output is DETERMINISTIC, because it becomes a `.map` file that has to
 *     diff cleanly against the last save or every export looks like a change.
 *
 * A note on the `claimed[]` tests inside the +X/+Z/+Y growth loops: they are
 * belt-and-braces, not load-bearing. The scan order (y, then z, then x) means a
 * box can only ever grow into cells the scan has not reached, so a claimed cell
 * inside a growth region is unreachable — verified by mutating each of those
 * tests away and watching the host check stay green, which is the honest reason
 * they are documented rather than presented as guards that fire. They are kept
 * because they cost one load per candidate and they are what makes the
 * disjointness property survive someone changing the scan order later; the
 * checks that DO fire on a broken merge are the type match and the extents.
 *
 * A claim bitmap over the whole grid would be 256*64*256/8 = 512 KB, which is
 * not available. So it is per-chunk and boxes never cross a chunk boundary. That
 * costs some brushes on a long wall (one per 16 blocks) and buys a 512-byte
 * bitmap; at kiln_map's 256-brush ceiling the trade still leaves room for a
 * 4096-block room, and the alternative is a data structure that does not fit.
 */
int kiln_voxel_boxes(const KilnVoxelWorld *w, KilnBrush *out, uint16_t cap,
                    const uint8_t *surface_of)
{
    const float B = (float)KILN_VOXEL_BLOCK_UNITS;
    uint32_t written = 0, needed = 0;
    uint8_t claimed[KILN_VOXEL_CHUNK_BLOCKS];

    for (int s = kiln_voxel_slot_first(w); s >= 0; s = kiln_voxel_slot_next(w, s)) {
        const KilnVoxelChunk *c = &w->chunks[s];
        memset(claimed, 0, sizeof claimed);
        int ox = c->cx * KILN_VOXEL_CHUNK;
        int oy = c->cy * KILN_VOXEL_CHUNK;
        int oz = c->cz * KILN_VOXEL_CHUNK;

        for (int y = 0; y < KILN_VOXEL_CHUNK; y++)
        for (int z = 0; z < KILN_VOXEL_CHUNK; z++)
        for (int x = 0; x < KILN_VOXEL_CHUNK; x++) {
            int bi = BI(x, y, z);
            uint8_t t = c->blocks[bi];
            if (t == KILN_VOXEL_AIR || claimed[bi]) continue;

            int wx = 1;
            while (x + wx < KILN_VOXEL_CHUNK) {
                int j = BI(x + wx, y, z);
                if (c->blocks[j] != t || claimed[j]) break;
                wx++;
            }

            int wz = 1;
            while (z + wz < KILN_VOXEL_CHUNK) {
                int ok = 1;
                for (int i = 0; i < wx && ok; i++) {
                    int j = BI(x + i, y, z + wz);
                    if (c->blocks[j] != t || claimed[j]) ok = 0;
                }
                if (!ok) break;
                wz++;
            }

            int wy = 1;
            while (y + wy < KILN_VOXEL_CHUNK) {
                int ok = 1;
                for (int k = 0; k < wz && ok; k++)
                    for (int i = 0; i < wx && ok; i++) {
                        int j = BI(x + i, y + wy, z + k);
                        if (c->blocks[j] != t || claimed[j]) ok = 0;
                    }
                if (!ok) break;
                wy++;
            }

            for (int j2 = 0; j2 < wy; j2++)
                for (int k = 0; k < wz; k++)
                    for (int i = 0; i < wx; i++)
                        claimed[BI(x + i, y + j2, z + k)] = 1;

            needed++;
            if (out && written < cap) {
                KilnBrush *br = &out[written];
                br->mins.v[0] = w->offset.v[0] + (float)(ox + x) * B;
                br->mins.v[1] = w->offset.v[1] + (float)(oy + y) * B;
                br->mins.v[2] = w->offset.v[2] + (float)(oz + z) * B;
                br->maxs.v[0] = br->mins.v[0] + (float)wx * B;
                br->maxs.v[1] = br->mins.v[1] + (float)wy * B;
                br->maxs.v[2] = br->mins.v[2] + (float)wz * B;
                br->surface = surface_of ? surface_of[t] : t;
                br->flags = 0;
                br->_pad[0] = br->_pad[1] = 0;
                written++;
            }
        }
    }

    if (needed > cap) return -(int)needed;
    return (int)written;
}

/* ── Reduction 2: surface -> merged quads ──────────────────────────────
 *
 * The standard greedy surface mesher, one 2D pass per face direction over the
 * chunk's 16 slices along that direction's normal axis. A face exists where the
 * block is solid and its neighbour along the normal is air — and the neighbour
 * is fetched through kiln_voxel_get on WORLD coordinates, which is what makes a
 * chunk seam invisible. Meshing a chunk against its own array instead treats
 * the chunk boundary as air and welds a wall across every seam; that reads as
 * "the level is made of boxes", which would in fact be true.
 */
int kiln_voxel_quads(const KilnVoxelWorld *w, int slot,
                    KilnVoxelQuad *out, uint32_t cap)
{
    if (slot < 0 || slot >= KILN_VOXEL_MAX_CHUNKS || !w->used[slot]) return 0;
    const KilnVoxelChunk *c = &w->chunks[slot];
    const int org[3] = { c->cx * KILN_VOXEL_CHUNK,
                         c->cy * KILN_VOXEL_CHUNK,
                         c->cz * KILN_VOXEL_CHUNK };

    uint32_t written = 0, needed = 0;
    /* mask[v][u] holds the block type whose face is exposed here, 0 for none. */
    uint8_t mask[KILN_VOXEL_CHUNK][KILN_VOXEL_CHUNK];

    for (uint8_t dir = 0; dir < KILN_VOXEL_DIRS; dir++) {
        const int au = DIR_AXES[dir][0], av = DIR_AXES[dir][1], an = DIR_AXES[dir][2];
        const int sgn = DIR_SIGN[dir];

        for (int n = 0; n < KILN_VOXEL_CHUNK; n++) {
            memset(mask, 0, sizeof mask);

            for (int v = 0; v < KILN_VOXEL_CHUNK; v++)
            for (int u = 0; u < KILN_VOXEL_CHUNK; u++) {
                int local[3];
                local[au] = u; local[av] = v; local[an] = n;
                uint8_t here = c->blocks[BI(local[0], local[1], local[2])];
                if (here == KILN_VOXEL_AIR) continue;

                int nb[3] = { org[0] + local[0], org[1] + local[1], org[2] + local[2] };
                nb[an] += sgn;
                if (kiln_voxel_get(w, nb[0], nb[1], nb[2]) != KILN_VOXEL_AIR) continue;
                mask[v][u] = here;
            }

            for (int v = 0; v < KILN_VOXEL_CHUNK; v++)
            for (int u = 0; u < KILN_VOXEL_CHUNK; u++) {
                uint8_t t = mask[v][u];
                if (!t) continue;

                int qw = 1;
                while (u + qw < KILN_VOXEL_CHUNK && mask[v][u + qw] == t) qw++;

                int qh = 1;
                while (v + qh < KILN_VOXEL_CHUNK) {
                    int ok = 1;
                    for (int i = 0; i < qw && ok; i++)
                        if (mask[v + qh][u + i] != t) ok = 0;
                    if (!ok) break;
                    qh++;
                }

                for (int j = 0; j < qh; j++)
                    for (int i = 0; i < qw; i++)
                        mask[v + j][u + i] = 0;

                needed++;
                if (out && written < cap) {
                    int local[3];
                    local[au] = u; local[av] = v; local[an] = n;
                    KilnVoxelQuad *q = &out[written];
                    q->x = (int16_t)(org[0] + local[0]);
                    q->y = (int16_t)(org[1] + local[1]);
                    q->z = (int16_t)(org[2] + local[2]);
                    q->dir = dir;
                    q->block = t;
                    q->w = (uint8_t)qw;
                    q->h = (uint8_t)qh;
                    q->_pad[0] = q->_pad[1] = 0;
                    written++;
                }
            }
        }
    }

    if (needed > cap) return -(int)needed;
    return (int)written;
}
