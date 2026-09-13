/* SPDX-License-Identifier: MIT
 *
 * kiln_map.c — see kiln_map.h for the model.
 */

#include "kiln_map.h"

#include <libdragon.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <malloc.h>
#include <float.h>

/* The capacities come from kiln_levelvocab.h, which is generated from
 * tools/schema/level_vocab.json — the same numbers tools/mapmaker/validate.py
 * reports a level against. They used to be typed here and typed again there,
 * and nothing compared them. The short local names are kept so the rest of
 * this file is untouched. */
#include "kiln_levelvocab.h"

#define MAX_CLASSNAMES KILN_LEVEL_MAX_CLASSNAMES
#define MAX_BRUSHES    KILN_LEVEL_MAX_BRUSHES
#define MAX_FACES      KILN_LEVEL_MAX_FACES
#define MAX_SPAWNS     KILN_LEVEL_MAX_SPAWNS
#define MAX_ENTITIES   KILN_LEVEL_MAX_ENTITIES

typedef struct {
    const char *name;
    uint16_t    profile_id;
} ClassnameReg;

static ClassnameReg g_classes[MAX_CLASSNAMES];
static int          g_class_count;

/* Brushes discarded because MAX_BRUSHES was reached. Reset by kiln_map_load,
 * reported by it. kiln_map_load is not reentrant, so a file-static is the
 * whole mechanism. */
static int          g_dropped_brushes;

void kiln_map_register_classname(const char *classname, uint16_t profile_id)
{
    if (g_class_count >= MAX_CLASSNAMES) {
        debugf("kiln_map: classname table full, ignoring '%s'\n", classname);
        return;
    }
    size_t n = strlen(classname);
    char *copy = malloc(n + 1);
    memcpy(copy, classname, n + 1);
    g_classes[g_class_count].name = copy;
    g_classes[g_class_count].profile_id = profile_id;
    g_class_count++;
}

static uint16_t profile_for_classname(const char *classname)
{
    for (int i = 0; i < g_class_count; i++) {
        if (strcmp(g_classes[i].name, classname) == 0)
            return g_classes[i].profile_id;
    }
    return 0xFFFFu;
}

/* 1-based line number of `at` within `buf`. Only ever called on a failure
 * path, so the linear scan costs nothing that matters — and a byte offset is
 * not something a person can act on without opening the file in an editor
 * that shows them. */
static int line_of(const char *buf, const char *at)
{
    int line = 1;
    for (const char *c = buf; c < at && *c; c++)
        if (*c == '\n') line++;
    return line;
}

static void skip_ws(const char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

// Skip whitespace and C++ line comments. .map files in this repo
// (and Quake .map files generally) start with a header preamble of
// `// SPDX-...` comments, and authors also leave inline notes between
// entities. The first non-comment, non-whitespace token has to be a
// `{` for the outer entity loop to recognise an entity; without this
// the parser sees `/` at offset 0, fails the `{` check, and exits
// with 0 brushes and 0 spawns — a degenerate success that the
// caller has no way to distinguish from a real empty map.
static void skip_ws_and_comments(const char **p)
{
    while (1) {
        skip_ws(p);
        if (**p != '/' || (*p)[1] != '/') return;
        while (**p && **p != '\n') (*p)++;
    }
}

static int read_token(const char **p, char *out, size_t out_sz)
{
    skip_ws(p);
    if (**p == '\0') return 0;
    size_t n = 0;
    if (**p == '"') {
        (*p)++;
        while (**p != '"' && **p != '\0' && n + 1 < out_sz) {
            out[n++] = **p;
            (*p)++;
        }
        if (**p == '"') (*p)++;
    } else if (**p == '{' || **p == '}' || **p == '(' || **p == ')') {
        out[n++] = **p;
        (*p)++;
    } else {
        while (**p != '\0' && !isspace((unsigned char)**p)
               && **p != '{' && **p != '}' && **p != '(' && **p != ')'
               && n + 1 < out_sz) {
            out[n++] = **p;
            (*p)++;
        }
    }
    out[n] = '\0';
    return n > 0 ? 1 : 0;
}

static int expect_token(const char **p, const char *tok)
{
    char tmp[64];
    if (!read_token(p, tmp, sizeof(tmp))) return 0;
    return strcmp(tmp, tok) == 0;
}

static void update_aabb(fm_vec3_t *minv, fm_vec3_t *maxv, fm_vec3_t pt)
{
    for (int i = 0; i < 3; i++) {
        if (pt.v[i] < minv->v[i]) minv->v[i] = pt.v[i];
        if (pt.v[i] > maxv->v[i]) maxv->v[i] = pt.v[i];
    }
}

/* ── Brush CSG ───────────────────────────────────────────────────────────────
 * A Quake brush is NOT a face list. Each line is three points ON A PLANE, and
 * the solid is the intersection of the half-spaces those planes bound. There
 * is no vertex anywhere in the file; the vertices have to be derived, exactly
 * as qbsp does when it compiles a .map.
 *
 * This file used to skip that step: it read the three points as three corners
 * of a face and completed the parallelogram p0,p1,p2,p0+p2-p1. The points are
 * conventionally one unit apart, so every 128-unit wall in every level here
 * rendered as a 1x2-unit patch at one corner, six per brush -- and nobody
 * noticed, because what reaches the screen comes from models and the brushes
 * are collision.
 *
 * The correct algorithm was already in this repo, on the host side:
 * tools/blender/quake_map.py's brush_to_faces / _intersect3 / _order_ring.
 * What follows is that algorithm in C, deliberately step for step so the two
 * can be read against each other:
 *
 *   1. every triple of planes that meets at a single point is a CANDIDATE
 *      vertex (the closed-form three-plane identity; a near-zero determinant
 *      means two are parallel, or all three share a line)
 *   2. a candidate survives only if it is inside or on EVERY other plane --
 *      most triples of a brush with more than four planes meet outside it
 *   3. a survivor belongs to each of its three generating planes; group by
 *      plane, sort into a ring by angle about the face's own centroid in the
 *      face's own 2D basis, and that ring IS the polygon
 *
 * O(planes^3) per brush, run once per level load. A brush is 6-20 planes.
 *
 * Two departures from the Python, both forced and both deliberate:
 *
 *   * The epsilons are larger. quake_map.py runs in double precision and uses
 *     1e-5; a float carries roughly 1e-4 of absolute slack at a coordinate of
 *     1024, so 1e-5 here would reject a brush's own corners as "outside" and
 *     silently shave vertices off faces. These are in MAP UNITS, and every
 *     .map in this repo is authored on an integer grid, so 1/32 of a unit sits
 *     far above the float noise and far below the smallest real feature.
 *
 *   * The angle sort uses a diamond angle, not atan2f. It is exactly monotonic
 *     in atan2 over the whole circle, costs one divide -- and, the reason that
 *     matters here, it is plain arithmetic, so the vertex ORDER is identical on
 *     the VR4300, on x86_64, under wasm32 and under qemu. nix/checks/refs/ has
 *     ONE reference capture shared by all four, and a libm call inside a sort
 *     key is exactly how one architecture comes to disagree about a tie.
 */

/* A face of a brush with P planes can have at most P-1 vertices. 32 planes is
 * already an elaborate brush for a console that reduces the whole thing to an
 * AABB for collision, and 16 vertices on one face is a 16-gon. Both overflows
 * are reported rather than clamped silently -- a brush that loses a plane
 * loses a wall you can walk through. */
/* Both from tools/schema/level_vocab.json via kiln_levelvocab.h, exactly as
 * MAX_BRUSHES is: tools/mapmaker/mapfmt.py refuses a brush past either, so a
 * brush this parser would silently truncate never reaches a ROM. They were
 * literals here when real CSG landed, which AGENTS.md forbids. */
#define MAX_BRUSH_PLANES KILN_LEVEL_MAX_BRUSH_PLANES
#define MAX_FACE_VERTS   KILN_LEVEL_MAX_FACE_VERTS

#define CSG_EPS_INSIDE   0.03125f   /* 1/32 unit: "on or behind" a plane      */
#define CSG_EPS_ONPLANE  0.03125f   /* 1/32 unit: this vertex lies on it      */
#define CSG_EPS_WELD     0.125f     /* 1/8 unit: same corner, twice           */
#define CSG_EPS_DET      1e-5f      /* unit normals, so this one is absolute  */

typedef struct {
    fm_vec3_t n;    /* outward unit normal */
    float     d;    /* n . x = d           */
} MapPlane;

/* Per-brush scratch. kiln_map_load is not reentrant (see g_dropped_brushes),
 * so one heap block for the whole load, freed with the parse -- ~7 KB, and
 * none of it resident afterwards. A stack local of this size is not safe on
 * this console and a file-static of it would be 7 KB of BSS in every ROM that
 * links kiln_map, whether or not it ever loads a map. */
typedef struct {
    MapPlane  planes[MAX_BRUSH_PLANES];
    fm_vec3_t verts[MAX_BRUSH_PLANES][MAX_FACE_VERTS];
    uint8_t   vcount[MAX_BRUSH_PLANES];
    int       plane_count;
    int       dropped_planes;
    int       dropped_verts;
} BrushWork;

static BrushWork *g_work;

/* Quake winding: p1,p2,p3 are CLOCKWISE seen from OUTSIDE the solid, so the
 * outward normal is cross(p3-p1, p2-p1) -- not the more obvious
 * cross(p2-p1, p3-p1), which is what this file used to compute. That sign was
 * wrong for as long as the module has existed: every brush face was handed the
 * RSP an inward normal, so the lighting term was negated on all of them.
 * tools/blender/quake_map.py's plane_normal_dist settles the convention
 * against the canonical 6-plane axial cube; this is the same derivation. */
static int plane_from_points(const fm_vec3_t pts[3], MapPlane *out)
{
    fm_vec3_t a, b, n;
    fm_vec3_sub(&a, &pts[2], &pts[0]);
    fm_vec3_sub(&b, &pts[1], &pts[0]);
    fm_vec3_cross(&n, &a, &b);
    if (fm_vec3_len2(&n) <= 1e-12f) return -1;   /* collinear points */
    fm_vec3_norm(&n, &n);
    out->n = n;
    out->d = fm_vec3_dot(&n, &pts[0]);
    return 0;
}

/* The single point common to three planes, or -1 if the 3x3 system is
 * singular:
 *   v = (d1(n2 x n3) + d2(n3 x n1) + d3(n1 x n2)) / (n1 . (n2 x n3))
 * The standard identity every Quake-family compiler uses, not a Gaussian
 * solve -- three planes is not enough to justify a matrix routine. */
static int intersect3(const MapPlane *a, const MapPlane *b, const MapPlane *c,
                      fm_vec3_t *out)
{
    fm_vec3_t cbc, cca, cab;
    fm_vec3_cross(&cbc, &b->n, &c->n);
    const float det = fm_vec3_dot(&a->n, &cbc);
    if (det > -CSG_EPS_DET && det < CSG_EPS_DET) return -1;
    fm_vec3_cross(&cca, &c->n, &a->n);
    fm_vec3_cross(&cab, &a->n, &b->n);
    const float inv = 1.0f / det;
    for (int i = 0; i < 3; i++)
        out->v[i] = (a->d * cbc.v[i] + b->d * cca.v[i] + c->d * cab.v[i]) * inv;
    return 0;
}

static void add_vert(BrushWork *w, int plane, const fm_vec3_t *v)
{
    for (int i = 0; i < w->vcount[plane]; i++) {
        fm_vec3_t d;
        fm_vec3_sub(&d, v, &w->verts[plane][i]);
        if (fm_vec3_len2(&d) < CSG_EPS_WELD * CSG_EPS_WELD) return;
    }
    if (w->vcount[plane] >= MAX_FACE_VERTS) { w->dropped_verts++; return; }
    w->verts[plane][w->vcount[plane]++] = *v;
}

/* Monotonic in atan2(y, x) over the full circle, returning [0, 4) instead of
 * (-pi, pi]. Only the ORDER of the keys is used, so any strictly increasing
 * function of the angle sorts the ring identically -- and this one is four
 * compares and a divide with no libm and no per-architecture rounding. */
static float diamond_angle(float y, float x)
{
    if (y >= 0.0f) {
        if (x >= 0.0f) { const float s = x + y;  return s  > 0.0f ? y / s        : 0.0f; }
        else           { const float s = y - x;  return s  > 0.0f ? 1.0f - x / s : 1.0f; }
    } else {
        if (x <  0.0f) { const float s = -x - y; return s  > 0.0f ? 2.0f - y / s : 2.0f; }
        else           { const float s = x - y;  return s  > 0.0f ? 3.0f + x / s : 3.0f; }
    }
}

/* Sort one plane's surviving vertices into a ring, counter-clockwise as seen
 * from OUTSIDE (looking against the outward normal) -- the winding
 * t3d_tri_draw reads as front-facing, and the same one quake_map.py hands
 * Blender. Returns the vertex count, or 0 if the plane contributes no polygon
 * (a plane that never reaches the solid's surface, which is normal and not an
 * error: a brush can have fewer faces than planes). */
static int order_ring(BrushWork *w, int plane)
{
    fm_vec3_t *vs = w->verts[plane];
    const int n = w->vcount[plane];
    if (n < 3) return 0;

    fm_vec3_t centre = {{ 0, 0, 0 }};
    for (int i = 0; i < n; i++) fm_vec3_add(&centre, &centre, &vs[i]);
    fm_vec3_scale(&centre, &centre, 1.0f / (float)n);

    /* A 2D basis lying in the face's own plane. The first vertex picks the
     * zero angle, which is arbitrary but deterministic. */
    fm_vec3_t u;
    int seed = -1;
    for (int i = 0; i < n && seed < 0; i++) {
        fm_vec3_sub(&u, &vs[i], &centre);
        if (fm_vec3_len2(&u) > 1e-10f) seed = i;
    }
    if (seed < 0) return 0;             /* every vertex at the centroid */
    fm_vec3_norm(&u, &u);
    fm_vec3_t vax;
    fm_vec3_cross(&vax, &w->planes[plane].n, &u);

    float key[MAX_FACE_VERTS];
    for (int i = 0; i < n; i++) {
        fm_vec3_t d;
        fm_vec3_sub(&d, &vs[i], &centre);
        key[i] = diamond_angle(fm_vec3_dot(&d, &vax), fm_vec3_dot(&d, &u));
    }

    for (int i = 1; i < n; i++) {       /* insertion sort: n <= 16, stable */
        const float k = key[i];
        const fm_vec3_t v = vs[i];
        int j = i - 1;
        while (j >= 0 && key[j] > k) { key[j + 1] = key[j]; vs[j + 1] = vs[j]; j--; }
        key[j + 1] = k;
        vs[j + 1] = v;
    }
    return n;
}

/* Round, do not truncate. A bevelled brush's CSG vertex lands on 63.99998 and
 * a plain (int16_t) cast would put that wall a unit inside itself, which is
 * exactly the class of off-by-one this commit is removing elsewhere. Written
 * out rather than calling roundf() so the arithmetic is identical on every
 * architecture that shares nix/checks/refs/. */
static int16_t to_i16(float f)
{
    const float r = (f < 0.0f) ? f - 0.5f : f + 0.5f;
    if (r >=  32767.0f) return  32767;
    if (r <= -32768.0f) return -32768;
    return (int16_t)r;
}

static int parse_brush(const char **p, KilnBrush *brush, KilnMapFace *faces,
                       int *face_idx, int max_faces)
{
    if (!expect_token(p, "{")) return -1;

    BrushWork *w = g_work;
    w->plane_count = 0;
    w->dropped_planes = 0;
    w->dropped_verts = 0;
    memset(w->vcount, 0, sizeof w->vcount);

    /* The min/max of the PLANE POINTS, kept only as a fallback -- see the AABB
     * note at the end of this function for when it is used and why. */
    fm_vec3_t pt_mins = {{  FLT_MAX,  FLT_MAX,  FLT_MAX }};
    fm_vec3_t pt_maxs = {{ -FLT_MAX, -FLT_MAX, -FLT_MAX }};

    while (1) {
        skip_ws(p);
        if (**p == '}') { (*p)++; break; }

        /* Each plane is 3 points (each "( x y z )") followed by a texture name
         * and 5 UV/rotation/scale tokens. No outer parens wrap the line. */
        fm_vec3_t pts[3];
        for (int i = 0; i < 3; i++) {
            if (!expect_token(p, "(")) return -1;
            char tok[64];
            float v[3];
            for (int j = 0; j < 3; j++) {
                if (!read_token(p, tok, sizeof(tok))) return -1;
                v[j] = (float)atof(tok);
            }
            if (!expect_token(p, ")")) return -1;
            pts[i].v[0] = v[0];
            pts[i].v[1] = v[1];
            pts[i].v[2] = v[2];
            update_aabb(&pt_mins, &pt_maxs, pts[i]);
        }

        /* Texture name + 5 params (offset_x, offset_y, rotation, x_scale,
         * y_scale). Fixed-count read matches the Quake format exactly;
         * a malformed face fails cleanly rather than silently misaligning. */
        char tok[64];
        for (int i = 0; i < 6; i++) {
            if (!read_token(p, tok, sizeof(tok))) return -1;
        }

        /* Keep consuming tokens past the cap so the parser stays in sync with
         * the file -- the brush is wrong either way, but a desynced parser
         * takes the rest of the level down with it. */
        if (w->plane_count >= MAX_BRUSH_PLANES) { w->dropped_planes++; continue; }

        MapPlane pl;
        if (plane_from_points(pts, &pl) < 0) {
            debugf("kiln_map: degenerate plane (collinear points), skipped\n");
            continue;
        }
        w->planes[w->plane_count++] = pl;
    }

    if (w->dropped_planes)
        debugf("kiln_map: brush has more than MAX_BRUSH_PLANES (%d) planes; "
               "%d dropped, so this solid is open and you can walk out of it\n",
               MAX_BRUSH_PLANES, w->dropped_planes);

    /* 1+2: candidate vertices, kept only if inside every plane. */
    const int np = w->plane_count;
    for (int i = 0; i < np; i++) {
        for (int j = i + 1; j < np; j++) {
            for (int k = j + 1; k < np; k++) {
                fm_vec3_t v;
                if (intersect3(&w->planes[i], &w->planes[j], &w->planes[k], &v) < 0)
                    continue;
                int inside = 1;
                for (int q = 0; q < np && inside; q++)
                    if (fm_vec3_dot(&w->planes[q].n, &v) > w->planes[q].d + CSG_EPS_INSIDE)
                        inside = 0;
                if (!inside) continue;
                const int tri[3] = { i, j, k };
                for (int t = 0; t < 3; t++) {
                    const MapPlane *pl = &w->planes[tri[t]];
                    const float off = fm_vec3_dot(&pl->n, &v) - pl->d;
                    if (off < CSG_EPS_ONPLANE && off > -CSG_EPS_ONPLANE)
                        add_vert(w, tri[t], &v);
                }
            }
        }
    }

    if (w->dropped_verts)
        debugf("kiln_map: a brush face has more than MAX_FACE_VERTS (%d) "
               "vertices; %d dropped, so that face is missing a corner\n",
               MAX_FACE_VERTS, w->dropped_verts);

    /* 3: order each plane's survivors into a polygon and emit it. */
    fm_vec3_t mins = {{  FLT_MAX,  FLT_MAX,  FLT_MAX }};
    fm_vec3_t maxs = {{ -FLT_MAX, -FLT_MAX, -FLT_MAX }};
    int total_verts = 0;

    for (int pi = 0; pi < np; pi++) {
        const int nv = order_ring(w, pi);
        if (nv < 3) continue;           /* plane never reaches the surface */

        if (*face_idx >= max_faces) {
            debugf("kiln_map: MAX_FACES (%d) reached; the load is abandoned\n",
                   max_faces);
            return -1;
        }

        const fm_vec3_t *vs = w->verts[pi];
        for (int i = 0; i < nv; i++) update_aabb(&mins, &maxs, vs[i]);
        total_verts += nv;

        /* Two vertices per T3DVertPacked, so an odd polygon rounds up and the
         * spare slot repeats the last vertex: t3d_vert_load moves whole pairs
         * and the DMA does not know what a polygon is. The repeat is never
         * indexed by the fan below, so it costs one transform and nothing
         * else. */
        const int structs = (nv + 1) / 2;
        T3DVertPacked *v = malloc_uncached(sizeof(T3DVertPacked) * structs);
        if (!v) return -1;

        /* uint16_t, not uint8_t. t3d_vert_pack_normal returns a 5.6.5 packed
         * normal; assigning it into a byte kept only the low half of y and the
         * whole of z, and dropped x entirely -- which is why three of
         * quake_test.map's six faces used to arrive with normA == 0, i.e. no
         * normal at all. One word, and the only reason it survived is that a
         * zero normal shades as a plausible flat colour. */
        const uint16_t norm = t3d_vert_pack_normal(&w->planes[pi].n);

        for (int s = 0; s < structs; s++) {
            const fm_vec3_t *a = &vs[s * 2];
            const fm_vec3_t *b = &vs[(s * 2 + 1 < nv) ? s * 2 + 1 : nv - 1];
            v[s] = (T3DVertPacked){
                .posA  = { to_i16(a->v[0]), to_i16(a->v[1]), to_i16(a->v[2]) },
                .rgbaA = 0xFFFFFFFF,
                .normA = norm,
                .posB  = { to_i16(b->v[0]), to_i16(b->v[1]), to_i16(b->v[2]) },
                .rgbaB = 0xFFFFFFFF,
                .normB = norm,
            };
        }

        faces[*face_idx].verts      = v;
        faces[*face_idx].vert_count = (uint8_t)nv;
        faces[*face_idx].rgba       = 0xFFFFFFFF;
        (*face_idx)++;
    }

    /* ── The AABB, and the one place this still falls back ──────────────────
     * The AABB is what every consumer actually uses -- kiln_clip_set_world,
     * kiln_room's brush install, a game's PLAY screen -- so it is the field
     * that must never come back empty.
     *
     * With real CSG there are real vertices, so the box is the box: an
     * authored -64..64 brush measures -64..64. It used to measure -64..65,
     * because it was the min/max of the PLANE POINTS and the second and third
     * point of each plane are conventionally one unit along the surface. Every
     * collision box in every level here was a unit oversized, asymmetrically,
     * on whichever axes the convention happened to push outward.
     *
     * But a brush wound inside-out produces NO surviving candidates at all --
     * the reversed half-spaces intersect in the empty set -- and CLAUDE.md
     * records that six of the seven .map files committed here were exactly
     * that, loading perfectly because this parser was indifferent to winding.
     * They are canonicalised now, but `./dev map-canon` exists because the
     * next one will not be. Deriving the AABB from CSG output alone would turn
     * a rendering problem into a brush with no collision at all, which is the
     * failure mode that reads as "the player falls through the world" and
     * takes an afternoon to attribute.
     *
     * So: the true box when the CSG produced a solid, the old plane-point box
     * when it did not, and a debugf whenever it falls back.
     *
     * "Produced a solid" means a box with positive size on ALL THREE axes,
     * not merely "some vertices survived". A brush with ONE face wound
     * backwards, or with one plane missing, still yields a single quad: four
     * coplanar vertices whose box is zero-thick on one axis. This test was
     * first written `total_verts >= 4`, which accepted that quad and handed
     * the clip world a zero-thickness box -- exactly the "falls through the
     * world" failure the fallback exists for, and silently, because the
     * fallback never ran. It had only been tried by flipping EVERY face, the
     * one malformed case that leaves no vertices at all.
     * nix/checks/kiln-map-check.c now loads a brush with ONE face flipped. */
    int solid = total_verts >= 4 &&
                maxs.v[0] > mins.v[0] && maxs.v[1] > mins.v[1] &&
                maxs.v[2] > mins.v[2];
    if (solid) {
        brush->mins = mins;
        brush->maxs = maxs;
    } else {
        debugf("kiln_map: a brush produced no solid (%d planes, %d "
               "vertices) -- a face wound inside-out or a plane missing; its "
               "collision box falls back to the plane points and it may not "
               "render whole. Run ./dev map-canon on this file.\n",
               np, total_verts);
        brush->mins = pt_mins;
        brush->maxs = pt_maxs;
    }
    brush->surface = 0;
    brush->flags = 0;
    return 0;
}

static int parse_entity(const char **p, KilnDict *epairs, KilnBrush *brushes,
                        int *brush_idx, int max_brushes,
                        KilnMapFace *faces, int *face_idx, int max_faces,
                        KilnRoomSpawn *spawn_out, int *has_spawn)
{
    if (!expect_token(p, "{")) return -1;

    kiln_dict_init(epairs);
    *has_spawn = 0;

    while (1) {
        skip_ws_and_comments(p);
        if (**p == '}') { (*p)++; break; }

        if (**p == '{') {
            KilnBrush b;
            if (parse_brush(p, &b, faces, face_idx, max_faces) < 0) return -1;
            if (brushes && *brush_idx < max_brushes) {
                brushes[*brush_idx] = b;
                (*brush_idx)++;
            } else if (brushes) {
                /* Same silent drop the spawn table had, but this one loses
                 * GEOMETRY: the brush is gone from the render AND from the
                 * clip world, so the wall is invisible and you walk through
                 * it. Counted rather than logged per brush, and reported once
                 * by kiln_map_load -- and NOT by advancing *brush_idx, which
                 * would push brush_count past the array the world-AABB loop
                 * then walks. */
                g_dropped_brushes++;
            }
            continue;
        }

        char key[64], val[256];
        if (!read_token(p, key, sizeof(key))) return -1;
        if (!read_token(p, val, sizeof(val))) return -1;
        kiln_dict_set_auto(epairs, key, val);
    }

    const char *cn = kiln_dict_get_str(epairs, "classname", NULL);
    if (!cn) return 0; /* malformed entity, but not fatal */

    uint16_t pid = profile_for_classname(cn);
    if (pid != 0xFFFFu) {
        fm_vec3_t origin = kiln_dict_get_vec3(epairs, "origin", (fm_vec3_t){{0,0,0}});
        float yaw = (float)kiln_dict_get_int(epairs, "angle", 0);
        spawn_out->profile_id = pid;
        spawn_out->pos = origin;
        spawn_out->yaw = yaw * (M_PI / 180.0f);
        /* Copy the spawn args into the spawn template. */
        spawn_out->dict = *epairs;
        *has_spawn = 1;
    }

    return 0;
}

int kiln_map_load(KilnMap *out, const char *dfs_path)
{
    memset(out, 0, sizeof(*out));

    // libdragon's dfs_open takes a native DFS path ("maps/foo.map"), not
    // the newlib-style "rom:/maps/foo.map" prefix used by fopen et al.
    // Callers in this repo were passing the newlib form by convention and
    // seeing spurious ENOFILE — strip the prefix here so both forms work.
    const char *native_path = dfs_path;
    if (native_path && strncmp(native_path, "rom:/", 5) == 0) {
        native_path += 5;
    }

    int fh = dfs_open(native_path);
    if (fh < 0) {
        debugf("kiln_map: dfs_open(%s) failed\n", dfs_path);
        return -1;
    }
    uint32_t sz = dfs_size(fh);
    char *buf = malloc(sz + 1);
    if (!buf) {
        dfs_close(fh);
        return -1;
    }
    dfs_read(buf, 1, sz, fh);
    dfs_close(fh);
    buf[sz] = '\0';

    KilnBrush  *brushes  = malloc(sizeof(KilnBrush)  * MAX_BRUSHES);
    KilnMapFace *faces    = malloc(sizeof(KilnMapFace) * MAX_FACES);
    KilnRoomSpawn *spawns = malloc(sizeof(KilnRoomSpawn) * MAX_SPAWNS);
    /* The CSG scratch, ~7 KB, alive only for the duration of the parse. */
    g_work = malloc(sizeof(BrushWork));
    if (!brushes || !faces || !spawns || !g_work) {
        free(buf); free(brushes); free(faces); free(spawns);
        free(g_work); g_work = NULL;
        return -1;
    }

    const char *p = buf;
    int brush_count = 0;
    int face_count = 0;
    int spawn_count = 0;

    g_dropped_brushes = 0;

    fm_vec3_t world_min = {{ FLT_MAX, FLT_MAX, FLT_MAX }};
    fm_vec3_t world_max = {{ -FLT_MAX, -FLT_MAX, -FLT_MAX }};

    while (1) {
        skip_ws_and_comments(&p);
        if (*p == '\0') break;
        if (*p != '{') {
            debugf("kiln_map: %s:%d: expected '{', got '%c'\n",
                   dfs_path, line_of(buf, p), *p);
            break;
        }

        const char *entity_start = p;
        int brush_count_before = brush_count;
        KilnDict epairs;
        KilnRoomSpawn spawn;
        int has_spawn = 0;
        if (parse_entity(&p, &epairs, brushes, &brush_count, MAX_BRUSHES,
                         faces, &face_count, MAX_FACES,
                         &spawn, &has_spawn) < 0) {
            debugf("kiln_map: %s:%d: failed to parse entity\n",
                   dfs_path, line_of(buf, entity_start));
            break;
        }

        /* Update the world AABB from any brushes added by this entity. */
        for (int i = brush_count_before; i < brush_count; i++) {
            update_aabb(&world_min, &world_max, brushes[i].mins);
            update_aabb(&world_min, &world_max, brushes[i].maxs);
        }

        const char *cn = kiln_dict_get_str(&epairs, "classname", "");
        if (strcmp(cn, "worldspawn") != 0 && has_spawn) {
            if (spawn_count < MAX_SPAWNS) {
                spawns[spawn_count++] = spawn;
            } else {
                /* Dropping this silently is how a level loses its 65th entity
                 * and nobody finds out until someone notices a door that never
                 * opens. Every other capacity in this file reports; this one
                 * did not. */
                debugf("kiln_map: %s: MAX_SPAWNS (%d) reached, dropping '%s'\n",
                       dfs_path, MAX_SPAWNS, cn);
            }
        }
    }

    if (g_dropped_brushes)
        debugf("kiln_map: %s: MAX_BRUSHES (%d) reached; %d brush(es) dropped "
               "from both the mesh and the clip world\n",
               dfs_path, MAX_BRUSHES, g_dropped_brushes);

    free(buf);
    free(g_work);
    g_work = NULL;

    out->brushes     = brushes;
    out->brush_count = brush_count;
    out->faces       = faces;
    out->face_count  = face_count;
    out->spawns      = spawns;
    out->spawn_count = spawn_count;
    out->world_aabb_min = world_min;
    out->world_aabb_max = world_max;
    return 0;
}

void kiln_map_free(KilnMap *m)
{
    if (!m) return;
    for (int i = 0; i < m->face_count; i++) {
        if (m->faces[i].verts) free_uncached(m->faces[i].verts);
    }
    free(m->faces);
    free(m->brushes);
    free(m->spawns);
    m->brushes = NULL;
    m->faces = NULL;
    m->spawns = NULL;
}

void kiln_map_draw(const KilnMap *m)
{
    if (!m) return;
    for (int i = 0; i < m->face_count; i++) {
        const int nv = m->faces[i].vert_count;
        if (nv < 3 || !m->faces[i].verts) continue;

        /* One t3d_vert_load per face, as before -- but of the polygon's own
         * vertex count rounded up to the pair the DMA moves, not a fixed 8.
         * MAX_FACE_VERTS is 16, comfortably inside the 70-entry vertex cache,
         * so no face can straddle a load the way kiln_voxmesh's quads can. */
        t3d_vert_load(m->faces[i].verts, 0, (uint32_t)((nv + 1) & ~1));

        /* Triangle fan about vertex 0. The ring is wound counter-clockwise as
         * seen from outside the solid, so every triangle inherits that
         * winding and faces the same way -- which is the whole reason
         * order_ring sorts rather than just collecting. */
        for (int k = 2; k < nv; k++)
            t3d_tri_draw(0, k - 1, k);
        t3d_tri_sync();
    }
}
