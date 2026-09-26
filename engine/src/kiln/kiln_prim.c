/* SPDX-License-Identifier: MIT
 *
 * kiln_prim.c — see kiln_prim.h.
 */

#include "kiln_prim.h"
#include "kiln_sierp.h"

#include <malloc.h>
#include <string.h>

static int16_t to_s16(float v)
{
    if (v >  32767.0f) return  32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
}

static void pack_vert(T3DVertPacked *base, int vi, const int16_t pos[3],
                      uint32_t rgba, uint16_t norm)
{
    T3DVertPacked *p = &base[vi / 2];
    if (vi & 1) {
        memcpy(p->posB, pos, sizeof p->posB);
        p->normB = norm; p->rgbaB = rgba; p->stB[0] = p->stB[1] = 0;
    } else {
        memcpy(p->posA, pos, sizeof p->posA);
        p->normA = norm; p->rgbaA = rgba; p->stA[0] = p->stA[1] = 0;
    }
}

static int alloc_quads(KilnPrim *out, int quads)
{
    memset(out, 0, sizeof *out);
    /* Two vertices per entry, four per quad: two entries per quad. */
    out->verts = malloc_uncached(sizeof(T3DVertPacked) * (size_t)quads * 2);
    if (!out->verts) return -1;
    memset(out->verts, 0, sizeof(T3DVertPacked) * (size_t)quads * 2);
    out->quad_count = (uint16_t)quads;
    out->vert_count = (uint16_t)(quads * 4);
    return 0;
}

/* One quad centred on `c`, spanning ±u and ±v, facing `n`.
 *
 * Corners go (-u,-v) (+u,-v) (+u,+v) (-u,+v). With u x v pointing along n that
 * is counter-clockwise seen from the n side — the caller picks u and v so it
 * is, and kiln-prim asserts it for every face rather than trusting this
 * comment. */
static void emit_quad(T3DVertPacked *base, int q, fm_vec3_t c, fm_vec3_t u,
                      fm_vec3_t v, fm_vec3_t n, uint32_t rgba)
{
    static const int8_t SU[4] = { -1, +1, +1, -1 };
    static const int8_t SV[4] = { -1, -1, +1, +1 };
    const uint16_t norm = t3d_vert_pack_normal(&n);
    for (int i = 0; i < 4; i++) {
        int16_t pos[3];
        for (int k = 0; k < 3; k++)
            pos[k] = to_s16(c.v[k] + SU[i] * u.v[k] + SV[i] * v.v[k]);
        pack_vert(base, q * 4 + i, pos, rgba, norm);
    }
}

int kiln_prim_box(KilnPrim *out, fm_vec3_t offset, fm_vec3_t half,
                  uint32_t top, uint32_t side, uint32_t bottom)
{
    if (alloc_quads(out, 6) != 0) return -1;

    const float hx = half.v[0], hy = half.v[1], hz = half.v[2];
    /* Right-handed in-plane pairs: Y x Z = X, Z x X = Y, X x Y = Z. A face on
     * the negative side swaps its pair (Z x Y = -X), which keeps it wound
     * outward instead of mirroring it inside-out. */
    const fm_vec3_t X = {{ hx, 0, 0 }}, Y = {{ 0, hy, 0 }}, Z = {{ 0, 0, hz }};
    struct { fm_vec3_t n, u, v; uint32_t rgba; } f[6] = {
        { {{ +1, 0, 0 }}, Y, Z, side   },
        { {{ -1, 0, 0 }}, Z, Y, side   },
        { {{ 0, +1, 0 }}, Z, X, top    },
        { {{ 0, -1, 0 }}, X, Z, bottom },
        { {{ 0, 0, +1 }}, X, Y, side   },
        { {{ 0, 0, -1 }}, Y, X, side   },
    };
    for (int i = 0; i < 6; i++) {
        fm_vec3_t c = offset;
        c.v[0] += f[i].n.v[0] * hx;
        c.v[1] += f[i].n.v[1] * hy;
        c.v[2] += f[i].n.v[2] * hz;
        emit_quad(out->verts, i, c, f[i].u, f[i].v, f[i].n, f[i].rgba);
    }

    data_cache_hit_writeback(out->verts, sizeof(T3DVertPacked) * 12);
    return 0;
}

static void cross3(fm_vec3_t *o, const fm_vec3_t *a, const fm_vec3_t *b)
{
    o->v[0] = a->v[1] * b->v[2] - a->v[2] * b->v[1];
    o->v[1] = a->v[2] * b->v[0] - a->v[0] * b->v[2];
    o->v[2] = a->v[0] * b->v[1] - a->v[1] * b->v[0];
}

static float dot3(const fm_vec3_t *a, const fm_vec3_t *b)
{
    return a->v[0] * b->v[0] + a->v[1] * b->v[1] + a->v[2] * b->v[2];
}

static void emit_tri_quad(T3DVertPacked *base, int q,
                          fm_vec3_t a, fm_vec3_t b, fm_vec3_t c,
                          fm_vec3_t inward, uint32_t rgba)
{
    fm_vec3_t e1 = {{ b.v[0] - a.v[0], b.v[1] - a.v[1], b.v[2] - a.v[2] }};
    fm_vec3_t e2 = {{ c.v[0] - a.v[0], c.v[1] - a.v[1], c.v[2] - a.v[2] }};
    fm_vec3_t n;
    cross3(&n, &e1, &e2);
    fm_vec3_t to_in = {{ inward.v[0] - a.v[0], inward.v[1] - a.v[1], inward.v[2] - a.v[2] }};
    if (dot3(&n, &to_in) > 0.0f) {
        fm_vec3_t tmp = b; b = c; c = tmp;
        n.v[0] = -n.v[0]; n.v[1] = -n.v[1]; n.v[2] = -n.v[2];
    }
    fm_vec3_norm(&n, &n);
    const uint16_t pn = t3d_vert_pack_normal(&n);
    const fm_vec3_t p[4] = { a, b, c, c };
    for (int i = 0; i < 4; i++) {
        int16_t pos[3] = { to_s16(p[i].v[0]), to_s16(p[i].v[1]), to_s16(p[i].v[2]) };
        pack_vert(base, q * 4 + i, pos, rgba, pn);
    }
}

static const uint32_t SIERP_FACE[4] = {
    0xFFC890FFu, 0xE09070FFu, 0x90B0E8FFu, 0x7088C0FFu,
};

static void emit_tet(T3DVertPacked *base, int tet_i, const KilnTet *t)
{
    fm_vec3_t inward = {{
        0.25f * (t->v[0].v[0] + t->v[1].v[0] + t->v[2].v[0] + t->v[3].v[0]),
        0.25f * (t->v[0].v[1] + t->v[1].v[1] + t->v[2].v[1] + t->v[3].v[1]),
        0.25f * (t->v[0].v[2] + t->v[1].v[2] + t->v[2].v[2] + t->v[3].v[2]),
    }};
    /* Face opposite vertex i. */
    static const int F[4][3] = { {1,2,3}, {0,3,2}, {0,1,3}, {0,2,1} };
    for (int f = 0; f < 4; f++)
        emit_tri_quad(base, tet_i * 4 + f,
                      t->v[F[f][0]], t->v[F[f][1]], t->v[F[f][2]],
                      inward, SIERP_FACE[f]);
}

int kiln_prim_tets(KilnPrim *out, const KilnTet *tets, int n)
{
    if (n < 0) n = 0;
    if (alloc_quads(out, n * 4) != 0) return -1;
    for (int i = 0; i < n; i++) emit_tet(out->verts, i, &tets[i]);
    data_cache_hit_writeback(out->verts,
                             sizeof(T3DVertPacked) * (size_t)out->quad_count * 2);
    return 0;
}

void kiln_prim_tets_update(KilnPrim *p, const KilnTet *tets, int n)
{
    if (!p || !p->verts || n * 4 != (int)p->quad_count) return;
    for (int i = 0; i < n; i++) emit_tet(p->verts, i, &tets[i]);
    data_cache_hit_writeback(p->verts,
                             sizeof(T3DVertPacked) * (size_t)p->quad_count * 2);
}

int kiln_prim_floor(KilnPrim *out, float extent, int cells,
                    uint32_t rgba_a, uint32_t rgba_b)
{
    if (cells < 1) cells = 1;
    if (cells > KILN_PRIM_FLOOR_MAX_CELLS) cells = KILN_PRIM_FLOOR_MAX_CELLS;
    if (alloc_quads(out, cells * cells) != 0) return -1;

    const float cell = 2.0f * extent / (float)cells;
    const fm_vec3_t up = {{ 0, 1, 0 }};
    const fm_vec3_t u  = {{ 0, 0, cell * 0.5f }};   /* Z x X = +Y */
    const fm_vec3_t v  = {{ cell * 0.5f, 0, 0 }};
    for (int j = 0; j < cells; j++) {
        for (int i = 0; i < cells; i++) {
            fm_vec3_t c = {{ -extent + (i + 0.5f) * cell, 0,
                             -extent + (j + 0.5f) * cell }};
            emit_quad(out->verts, j * cells + i, c, u, v, up,
                      ((i + j) & 1) ? rgba_b : rgba_a);
        }
    }

    data_cache_hit_writeback(out->verts,
                             sizeof(T3DVertPacked) * (size_t)out->quad_count * 2);
    return 0;
}

void kiln_prim_draw(const KilnPrim *p)
{
    if (!p || !p->verts || !p->quad_count) return;

    uint32_t drawn = 0;
    while (drawn < p->quad_count) {
        uint32_t batch = p->quad_count - drawn;
        if (batch * 4 > KILN_PRIM_BATCH) batch = KILN_PRIM_BATCH / 4;

        /* Offset in vertices; the source pointer walks in entries of two. */
        t3d_vert_load(p->verts + drawn * 2, 0, batch * 4);
        for (uint32_t i = 0; i < batch; i++) {
            const uint32_t b = i * 4;
            t3d_tri_draw(b, b + 1, b + 2);
            t3d_tri_draw(b, b + 2, b + 3);
        }
        t3d_tri_sync();
        drawn += batch;
    }
}

void kiln_prim_free(KilnPrim *p)
{
    if (!p) return;
    if (p->verts) free_uncached(p->verts);
    memset(p, 0, sizeof *p);
}

uint32_t kiln_prim_shade(uint32_t rgba, float k)
{
    uint32_t out = rgba & 0xFFu;
    for (int shift = 8; shift <= 24; shift += 8) {
        float c = (float)((rgba >> shift) & 0xFFu) * k;
        if (c > 255.0f) c = 255.0f;
        if (c < 0.0f) c = 0.0f;
        out |= (uint32_t)c << shift;
    }
    return out;
}

void kiln_prim_stage(KilnScene *s, color_t sky, float fog_near, float fog_far)
{
    s->clear_color = sky;

    /* Ambient: a third of the sky plus a floor of 52. It was 28 + sky/4, which
     * read well in host renders and near-black on the console: through a 16-bit
     * framebuffer and a CRT-style output, every floor and shadowed face in the
     * first Ares batch sat within a few steps of the clear colour. */
    s->ambient[0] = (uint8_t)(52 + sky.r / 3);
    s->ambient[1] = (uint8_t)(52 + sky.g / 3);
    s->ambient[2] = (uint8_t)(52 + sky.b / 3);
    s->ambient[3] = 0xFF;

    /* Key: warm, high, from the camera's usual (-Z) side of the scene.
     *
     * Direction points TOWARD the light: Tiny3D lights a vertex by
     * +dot(normal, dir), so a light overhead has POSITIVE y. This preset once
     * shipped pointing down, tuned against a host renderer that had the sign
     * backwards — on console every floor and box top sat at bare ambient
     * while wall sides looked fine. kiln-prim samples a floor pixel so it
     * cannot come back. */
    s->light_color[0] = 0xFF; s->light_color[1] = 0xE8;
    s->light_color[2] = 0xC8; s->light_color[3] = 0xFF;
    s->light_dir = (fm_vec3_t){{ 0.40f, 0.80f, -0.45f }};
    fm_vec3_norm(&s->light_dir, &s->light_dir);

    /* Rim: cool and weaker, from behind and the other side, so the faces the
     * key cannot see still separate from each other. */
    s->lights[0].color[0] = 0x58; s->lights[0].color[1] = 0x78;
    s->lights[0].color[2] = 0xB8; s->lights[0].color[3] = 0xFF;
    s->lights[0].dir = (fm_vec3_t){{ -0.60f, 0.35f, 0.70f }};
    fm_vec3_norm(&s->lights[0].dir, &s->lights[0].dir);
    s->light_count = 2;

    if (fog_far > fog_near) kiln_scene_set_fog(s, sky, fog_near, fog_far);
    else kiln_scene_disable_fog(s);
}
