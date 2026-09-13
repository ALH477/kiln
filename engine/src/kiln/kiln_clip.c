/* SPDX-License-Identifier: MIT
 *
 * kiln_clip.c — see kiln_clip.h for the model.
 */

#include "kiln_clip.h"

#include <libdragon.h> /* debugf, assertf */
#include <stddef.h>
#include <string.h>

/* Single-precision slab epsilon. At s16.16 world scale, positions are ~10^3;
 * 1e-3 (1 mm) silences the contact jitter that a tighter epsilon produces
 * without being perceptible to a player. See the header comment. */
#define CLIP_EPS (1e-3f)

static const KilnBrush *g_brushes;
static uint16_t g_brush_count;

/* ── Broadphase grid ────────────────────────────────────────────────────
 *
 * A 2D XZ uniform grid over the installed brushes. Built fresh on every
 * kiln_clip_set_world call when g_broadphase is on; freed when it's off or
 * when the world is cleared. Storage is two fixed static arrays so there is
 * no allocation on a 4 MB console — the cell headers are 256 entries of
 * {offset,count} and the brush-index pool is 512 entries. A brush that
 * straddles cell boundaries is listed in every cell it overlaps, so a trace
 * must dedup by skipping brush indices it has already tested (cheap: a
 * 16-bit "last-tested" stamp per brush in g_tested[], advanced per trace).
 *
 * Y is ignored — OoT rooms are short and a 3D grid at 16^3 = 4096 cells with
 * bucket storage is 8+ KB before any brushes, which is too much of a 4 MB
 * budget for a feature that's opt-in. The 2D grid catches the same brushes
 * for the XZ-dominant motion OoT players actually do.
 */
#define CLIP_GRID_SIDE    16
#define CLIP_GRID_CELLS   (CLIP_GRID_SIDE * CLIP_GRID_SIDE)   /* 256  */
#define CLIP_GRID_POOL    512

typedef struct {
    uint16_t offset;
    uint16_t count;
} KilnClipCell;

static KilnClipCell g_cell[CLIP_GRID_CELLS];
static uint16_t    g_cell_pool[CLIP_GRID_POOL];
static uint16_t    g_cell_pool_count;

static float g_cell_size = 128.0f;
static float g_grid_origin_x = 0.0f;
static float g_grid_origin_z = 0.0f;
static int   g_broadphase = 0;

/* Per-brush "last trace that tested me" stamp, used to dedup brushes that
 * appear in multiple cells. Advanced once per top-level trace call. */
static uint16_t g_tested[CLIP_GRID_POOL];
static uint16_t g_trace_stamp = 0;

/* Debug counter for the physics-demo HUD. */
static uint16_t g_last_trace_brushes;

void kiln_clip_set_broadphase(int enabled)
{
    g_broadphase = enabled ? 1 : 0;
    /* Rebuild (or clear) the grid against the currently-installed world. */
    if (g_broadphase) {
        kiln_clip_set_world(g_brushes, g_brush_count);
    } else {
        g_cell_pool_count = 0;
        for (uint16_t i = 0; i < CLIP_GRID_CELLS; i++) {
            g_cell[i].offset = 0;
            g_cell[i].count  = 0;
        }
    }
}

uint16_t kiln_clip_last_trace_brushes(void)
{
    return g_last_trace_brushes;
}

uint16_t kiln_clip_world_count(void)
{
    return g_brush_count;
}

static inline int cell_index_for(float x, float z)
{
    int cx = (int)((x - g_grid_origin_x) / g_cell_size);
    int cz = (int)((z - g_grid_origin_z) / g_cell_size);
    if (cx < 0) cx = 0; else if (cx >= CLIP_GRID_SIDE) cx = CLIP_GRID_SIDE - 1;
    if (cz < 0) cz = 0; else if (cz >= CLIP_GRID_SIDE) cz = CLIP_GRID_SIDE - 1;
    return cz * CLIP_GRID_SIDE + cx;
}

/* Build the grid from the installed brushes. Called from kiln_clip_set_world
 * when g_broadphase is on. Cell size = max(world_extent_x, world_extent_z)/8,
 * clamped to [32, 256] — small worlds get small cells (more selective), big
 * worlds get big cells (so 16x16 still covers them). Origin is the world
 * min corner so brush coordinates map cleanly into [0, side). */
static void build_grid(const KilnBrush *brushes, uint16_t count)
{
    g_cell_pool_count = 0;
    for (uint16_t i = 0; i < CLIP_GRID_CELLS; i++) {
        g_cell[i].offset = 0;
        g_cell[i].count  = 0;
    }
    if (count == 0) return;

    fm_vec3_t wmin = brushes[0].mins;
    fm_vec3_t wmax = brushes[0].maxs;
    for (uint16_t i = 1; i < count; i++) {
        if (brushes[i].mins.v[0] < wmin.v[0]) wmin.v[0] = brushes[i].mins.v[0];
        if (brushes[i].mins.v[2] < wmin.v[2]) wmin.v[2] = brushes[i].mins.v[2];
        if (brushes[i].maxs.v[0] > wmax.v[0]) wmax.v[0] = brushes[i].maxs.v[0];
        if (brushes[i].maxs.v[2] > wmax.v[2]) wmax.v[2] = brushes[i].maxs.v[2];
    }
    float ex = wmax.v[0] - wmin.v[0];
    float ez = wmax.v[2] - wmin.v[2];
    float ext = ex > ez ? ex : ez;
    float cs  = ext / 8.0f;
    if (cs < 32.0f)   cs = 32.0f;
    if (cs > 256.0f)  cs = 256.0f;
    g_cell_size    = cs;
    g_grid_origin_x = wmin.v[0];
    g_grid_origin_z = wmin.v[2];

    /* First pass: count brushes per cell. */
    for (uint16_t i = 0; i < count; i++) {
        int cmin = cell_index_for(brushes[i].mins.v[0], brushes[i].mins.v[2]);
        int cmax = cell_index_for(brushes[i].maxs.v[0], brushes[i].maxs.v[2]);
        for (int cz = cmin / CLIP_GRID_SIDE; cz <= cmax / CLIP_GRID_SIDE; cz++) {
            for (int cx = cmin % CLIP_GRID_SIDE; cx <= cmax % CLIP_GRID_SIDE; cx++) {
                g_cell[cz * CLIP_GRID_SIDE + cx].count++;
            }
        }
    }
    /* Compact: convert per-cell counts into offsets into the pool, then
     * place brush indices. Caps are asserted — if a world ever exceeds
     * CLIP_GRID_POOL entries (counting brushes once per cell they overlap),
     * the user bumps the cap or turns broadphase off. */
    uint16_t acc = 0;
    for (uint16_t i = 0; i < CLIP_GRID_CELLS; i++) {
        g_cell[i].offset = acc;
        acc += g_cell[i].count;
        g_cell[i].count = 0; /* reset for the placement pass */
    }
    assertf(acc <= CLIP_GRID_POOL,
            "kiln_clip: broadphase pool overflow %u > %u — world too dense "
            "for the 16x16 grid, raise CLIP_GRID_POOL or disable broadphase",
            acc, CLIP_GRID_POOL);
    /* `acc` is used below to set g_cell_pool_count, so it is no longer just an
     * assert argument — the (void) cast that used to be here was covering for
     * a release build where assertf compiles away. */

    /* Placement pass. The write MUST go to this cell's own window,
     * `offset + count` — not to a running global cursor.
     *
     * It used to be `g_cell_pool[g_cell_pool_count++] = i`, which threw away
     * the offsets the compaction pass above had just computed: indices landed
     * in brush-iteration order while the trace walk reads cell `k`'s window as
     * `g_cell_pool[c->offset .. c->offset + c->count)`. Every cell therefore
     * saw *some* brushes, just not the ones overlapping it — so the broadphase
     * did not merely miss collisions, it tested the wrong geometry and could
     * report a hit against a brush nowhere near the sweep.
     *
     * The symptom was asymmetric and that is what made it survive: in a
     * four-walled room, traces along Z happened to land on the right brushes
     * and traces along X did not, so a player walked through two of the four
     * walls. Broadphase is opt-in and default-off, which is why no shipped ROM
     * hit it — examples/physics-demo can toggle it on, and would have.
     * Found by nix/checks/kiln-logic.nix's flat-vs-grid equivalence sweep. */
    for (uint16_t i = 0; i < count; i++) {
        int cmin = cell_index_for(brushes[i].mins.v[0], brushes[i].mins.v[2]);
        int cmax = cell_index_for(brushes[i].maxs.v[0], brushes[i].maxs.v[2]);
        for (int cz = cmin / CLIP_GRID_SIDE; cz <= cmax / CLIP_GRID_SIDE; cz++) {
            for (int cx = cmin % CLIP_GRID_SIDE; cx <= cmax % CLIP_GRID_SIDE; cx++) {
                KilnClipCell *c = &g_cell[cz * CLIP_GRID_SIDE + cx];
                g_cell_pool[c->offset + c->count] = i;
                c->count++;
            }
        }
    }
    /* The pool is exactly as full as the compaction pass predicted. Tracked
     * because kiln_clip_set_broadphase(0) zeroes it as its "grid is not built"
     * marker. */
    g_cell_pool_count = acc;
}

void kiln_clip_set_world(const KilnBrush *brushes, uint16_t count)
{
    g_brushes = brushes;
    g_brush_count = count;
    if (g_broadphase) {
        build_grid(brushes, count);
    }
}

/* Per-brush slab test. Updates *best_t / *best_n / *best_surf if this brush
 * is the closest hit so far. Returns nothing — the caller owns the best-set.
 * Factored out so both the flat walk and the grid walk share the same slab
 * math; this is the entire cost of a trace per brush. */
static inline void slab_test_brush(uint16_t i,
                                   fm_vec3_t start, fm_vec3_t delta,
                                   fm_vec3_t mins, fm_vec3_t maxs,
                                   float *best_t, fm_vec3_t *best_n,
                                   uint8_t *best_surf)
{
    const KilnBrush *b = &g_brushes[i];

    /* Minkowski-expand the brush by the box: emins = b.mins - box.maxs,
     * emaxs = b.maxs - box.mins. The box's center then traces a ray
     * against [emins, emaxs]. */
    fm_vec3_t emins, emaxs;
    emins.v[0] = b->mins.v[0] - maxs.v[0];
    emins.v[1] = b->mins.v[1] - maxs.v[1];
    emins.v[2] = b->mins.v[2] - maxs.v[2];
    emaxs.v[0] = b->maxs.v[0] - mins.v[0];
    emaxs.v[1] = b->maxs.v[1] - mins.v[1];
    emaxs.v[2] = b->maxs.v[2] - mins.v[2];

    float tmin = 0.0f;
    float tmax = 1.0f;
    fm_vec3_t hit_n = (fm_vec3_t){ { 0, 0, 0 } };

    for (int a = 0; a < 3; a++) {
        if (delta.v[a] > -CLIP_EPS && delta.v[a] < CLIP_EPS) {
            /* Parallel to this slab. If the start center is outside the
             * expanded slab on this axis, the box never overlaps the
             * brush on this axis → no hit.
             *
             * TOUCHING is outside. A box standing on a floor sits exactly on
             * its face (the slide's CLIP_EPS nudge leaves it there), and this
             * used to widen the slab by CLIP_EPS instead of narrowing it, so
             * resting contact counted as overlap. The moving axes then all
             * entered at t <= 0, which left tmin at 0 and the normal at zero:
             * a fraction-0 hit that kiln_clip_slide cannot clip anything off,
             * so a box on a floor could not move along it at all. kiln_fpscam
             * lands with a vertical slide and walks with a flat one, so the
             * fps player could turn but never take a step. kiln-logic pins it. */
            if (start.v[a] <= emins.v[a] + CLIP_EPS ||
                start.v[a] >= emaxs.v[a] - CLIP_EPS) {
                return; /* no hit on this brush */
            }
            continue;
        }
        float inv_d = 1.0f / delta.v[a];
        float t1 = (emins.v[a] - start.v[a]) * inv_d;
        float t2 = (emaxs.v[a] - start.v[a]) * inv_d;
        float n_sign;
        if (t1 > t2) {
            float tmp = t1; t1 = t2; t2 = tmp;
            n_sign = +1.0f; /* entered from the max side */
        } else {
            n_sign = -1.0f; /* entered from the min side */
        }
        if (t1 > tmin) {
            tmin = t1;
            hit_n = (fm_vec3_t){ { 0, 0, 0 } };
            hit_n.v[a] = n_sign;
        }
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return; /* no overlap on this brush */
    }

    if (tmin > tmax) return;       /* no overlap */
    if (tmin >= *best_t) return;   /* not the closest hit so far */
    if (tmin < -CLIP_EPS) return;  /* already inside / behind; skip */

    *best_t = tmin;
    *best_n = hit_n;
    *best_surf = b->surface;
}

/* Slab-method swept AABB vs AABB. See kiln_clip.h for the model. Walks either
 * the flat brush array (broadphase off) or the grid cells overlapped by the
 * swept AABB's XZ footprint (broadphase on). The grid path dedups brushes
 * that straddle cell boundaries via g_tested[]. */
static KilnTrace sweep_box(fm_vec3_t start, fm_vec3_t end,
                          fm_vec3_t mins, fm_vec3_t maxs)
{
    KilnTrace tr;
    tr.fraction = 1.0f;
    tr.endpos = end;
    tr.normal = (fm_vec3_t){ { 0, 0, 0 } };
    tr.hitsurface = 0;
    g_last_trace_brushes = 0;

    if (g_brush_count == 0) return tr;

    fm_vec3_t delta;
    fm_vec3_sub(&delta, &end, &start);

    float best_t = 1.0f;
    fm_vec3_t best_n = (fm_vec3_t){ { 0, 0, 0 } };
    uint8_t best_surf = 0;

    if (g_broadphase) {
        /* Swept AABB XZ footprint: union of start and end box footprints. */
        float xmin = start.v[0] + mins.v[0]; float t = end.v[0] + mins.v[0]; if (t < xmin) xmin = t;
        float xmax = start.v[0] + maxs.v[0];            t = end.v[0] + maxs.v[0]; if (t > xmax) xmax = t;
        float zmin = start.v[2] + mins.v[2];            t = end.v[2] + mins.v[2]; if (t < zmin) zmin = t;
        float zmax = start.v[2] + maxs.v[2];            t = end.v[2] + maxs.v[2]; if (t > zmax) zmax = t;
        int cmin = cell_index_for(xmin, zmin);
        int cmax = cell_index_for(xmax, zmax);
        int cz0 = cmin / CLIP_GRID_SIDE, cz1 = cmax / CLIP_GRID_SIDE;
        int cx0 = cmin % CLIP_GRID_SIDE, cx1 = cmax % CLIP_GRID_SIDE;

        g_trace_stamp++;
        if (g_trace_stamp == 0) {
            memset(g_tested, 0, sizeof(g_tested));
            g_trace_stamp = 1;
        }

        for (int cz = cz0; cz <= cz1; cz++) {
            for (int cx = cx0; cx <= cx1; cx++) {
                KilnClipCell *c = &g_cell[cz * CLIP_GRID_SIDE + cx];
                for (uint16_t k = 0; k < c->count; k++) {
                    uint16_t i = g_cell_pool[c->offset + k];
                    if (g_tested[i] == g_trace_stamp) continue;
                    g_tested[i] = g_trace_stamp;
                    g_last_trace_brushes++;
                    slab_test_brush(i, start, delta, mins, maxs,
                                    &best_t, &best_n, &best_surf);
                }
            }
        }
    } else {
        for (uint16_t i = 0; i < g_brush_count; i++) {
            g_last_trace_brushes++;
            slab_test_brush(i, start, delta, mins, maxs,
                            &best_t, &best_n, &best_surf);
        }
    }

    if (best_t < 1.0f) {
        if (best_t < 0.0f) best_t = 0.0f;
        tr.fraction = best_t;
        tr.normal = best_n;
        tr.hitsurface = best_surf;
        /* endpos = start + delta * fraction */
        tr.endpos.v[0] = start.v[0] + delta.v[0] * best_t;
        tr.endpos.v[1] = start.v[1] + delta.v[1] * best_t;
        tr.endpos.v[2] = start.v[2] + delta.v[2] * best_t;
    }
    return tr;
}

KilnTrace kiln_clip_box(fm_vec3_t start, fm_vec3_t end,
                      fm_vec3_t mins, fm_vec3_t maxs)
{
    return sweep_box(start, end, mins, maxs);
}

KilnTrace kiln_clip_ray(fm_vec3_t start, fm_vec3_t end)
{
    /* Rays always flat-walk, even with broadphase on — see the header comment
     * in kiln_clip.h: rays are 1/frame (camera boom, line-of-sight) and grid-
     * ray traversal (Amanatides-Woo) is more code than the win warrants. The
     * flat walk on a typical world is a few hundred compares. */
    fm_vec3_t zero = (fm_vec3_t){ { 0, 0, 0 } };
    int saved = g_broadphase;
    g_broadphase = 0;
    KilnTrace tr = sweep_box(start, end, zero, zero);
    g_broadphase = saved;
    return tr;
}

fm_vec3_t kiln_clip_slide(fm_vec3_t pos, fm_vec3_t vel,
                         fm_vec3_t mins, fm_vec3_t maxs,
                         int max_iter)
{
    if (max_iter < 1) max_iter = 1;

    fm_vec3_t cur = pos;
    fm_vec3_t remaining = vel;

    for (int it = 0; it < max_iter; it++) {
        /* If the remaining displacement is near-zero, we're done — avoid a
         * divide-by-zero in the slab test's `1/delta` per axis. */
        float r2 = remaining.v[0] * remaining.v[0]
                 + remaining.v[1] * remaining.v[1]
                 + remaining.v[2] * remaining.v[2];
        if (r2 < CLIP_EPS * CLIP_EPS) break;

        fm_vec3_t end;
        end.v[0] = cur.v[0] + remaining.v[0];
        end.v[1] = cur.v[1] + remaining.v[1];
        end.v[2] = cur.v[2] + remaining.v[2];

        KilnTrace tr = sweep_box(cur, end, mins, maxs);
        cur = tr.endpos;

        if (tr.fraction >= 1.0f) break; /* clean full move */

        /* Clip remaining velocity along the contact normal: remove the
         * component going into the wall. v -= (v·n) * n. The 1e-3 nudge
         * along the normal keeps us off the contact plane so the next trace
         * doesn't re-hit the same brush at fraction 0. */
        float vn = remaining.v[0] * tr.normal.v[0]
                 + remaining.v[1] * tr.normal.v[1]
                 + remaining.v[2] * tr.normal.v[2];
        remaining.v[0] -= vn * tr.normal.v[0];
        remaining.v[1] -= vn * tr.normal.v[1];
        remaining.v[2] -= vn * tr.normal.v[2];

        /* Scale remaining by the unused fraction of this trace, since `cur`
         * only advanced by `tr.fraction` of the displacement we traced. */
        float rest = 1.0f - tr.fraction;
        remaining.v[0] *= rest;
        remaining.v[1] *= rest;
        remaining.v[2] *= rest;

        /* Nudge out along the normal to avoid re-contacting the same face. */
        cur.v[0] += tr.normal.v[0] * CLIP_EPS;
        cur.v[1] += tr.normal.v[1] * CLIP_EPS;
        cur.v[2] += tr.normal.v[2] * CLIP_EPS;
    }
    return cur;
}

KilnTrace kiln_clip_ground(fm_vec3_t pos, fm_vec3_t mins, fm_vec3_t maxs)
{
    fm_vec3_t down = (fm_vec3_t){ {
        pos.v[0], pos.v[1] - 2.0f, pos.v[2],
    } };
    return sweep_box(pos, down, mins, maxs);
}