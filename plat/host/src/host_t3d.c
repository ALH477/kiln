/* SPDX-License-Identifier: MIT
 *
 * host_t3d.c — the 3D half of the host backend.
 *
 * Tiny3D's API on the console is a front end for RSP microcode: a vertex load
 * is a DMA into a 70-entry cache, a triangle is a command in an RSPQ block,
 * and the matrices are s16.16 because that is the format the ucode reads. None
 * of that exists here, so this is a reimplementation of the API's semantics
 * over the software rasteriser in host_gfx.c.
 *
 * ── Two decisions that make it useful rather than merely working ───────
 *
 * 1. The fixed-point round trip is HONOURED, not skipped. t3d_mat4_to_fixed
 *    exists because the RSP wants s16.16, and that conversion loses precision.
 *    A host that kept float matrices throughout would be MORE precise than the
 *    console and would therefore disagree with it about exactly the things
 *    precision decides — z-fighting on coplanar surfaces, a vertex landing on
 *    one side of a pixel centre or the other. So the conversion quantises for
 *    real, and t3d_matrix_push reads the quantised value back.
 *
 *    Vertex positions get the same treatment for free: T3DVertPacked's posA is
 *    an int16 that the ucode uses as the INTEGER PART of an s16.16, so world
 *    coordinates are whole numbers and are bounded to +/-32767. That is a real
 *    constraint on content (at 64 units per metre it is +/-512 m) and it is
 *    asserted rather than silently wrapped.
 *
 * 2. Every RSP limit that is silent on hardware is an assert here. The vertex
 *    cache is 70 entries and overrunning it on console wraps the DMA and
 *    corrupts geometry with no diagnostic at all; kiln_voxmesh batches 68 for
 *    precisely that reason. The matrix stack has a fixed depth. Those are the
 *    limits a host build can genuinely check, and checking them is the
 *    argument for having one — see CLAUDE.md on the host being STRICTER than
 *    the console, not laxer.
 *
 * ── What it still cannot tell you ─────────────────────────────────────
 * Fill rate, which is the console's actual binding constraint. It counts
 * triangles and pixels so the number is at least available, but a frame that
 * renders here is not evidence that the RDP could draw it in 16 ms.
 */
#include <t3d/t3d.h>
#include <kiln_host.h>

#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "host_internal.h"

#define MATRIX_STACK 16

/* ── The vertex cache holds TRANSFORMED vertices, not model-space ones ──
 * This is the semantic that rigid skinning rests on, and it is easy to get
 * wrong in a way only a skinned model exposes. Tiny3D's t3d_model_draw_object
 * says it outright: "load vertices, this will already do T&L (so
 * matrices/fog/lighting must be set before)". The transform happens at LOAD
 * time, against whatever matrix is current, and the cache accumulates results.
 *
 * That is how one-bone-per-vertex skinning works with no per-vertex weights:
 * gltf_to_t3d splits the mesh into one T3DObjectPart per bone, each carrying a
 * matrixIdx, and the draw loop pushes that bone's matrix, loads that part's
 * vertices into its own slice of the cache, and only then draws indices
 * spanning all of them. assets/skel_test.gltf is exactly this — part 0 has
 * matrixIdx 1 and draws nothing, part 1 has matrixIdx 0 and draws 72 indices
 * across both slices.
 *
 * Transforming at DRAW time instead is indistinguishable for every
 * single-matrix consumer (kiln_map, kiln_voxmesh, an unskinned model) and
 * silently collapses a skinned model onto its last bone. */
typedef struct {
    float   x, y, z, w;     /* clip space */
    uint8_t r, g, b, a;     /* lit, and fogged */
    float   sow, tow;       /* s/w, t/w for perspective-correct UV */
} Out;

static Out        g_cache[T3D_VERTEX_CACHE];
static int        g_cache_valid[T3D_VERTEX_CACHE];

/* Defined below, beside the lighting it invokes; declared here because
 * t3d_vert_load is where it is called from. */
static void transform_vertex(const fm_vec3_t *pos, const fm_vec3_t *nrm,
                             uint32_t rgba, float s, float t, Out *o);

static fm_mat4_t  g_stack[MATRIX_STACK];
static int        g_depth;                 /* 0 = identity only */
static T3DViewport *g_vp;
static enum T3DDrawFlags g_flags = T3D_FLAG_DEPTH | T3D_FLAG_SHADED;

static uint8_t    g_ambient[4]  = { 255, 255, 255, 255 };
static int        g_light_count;
static struct { uint8_t color[4]; fm_vec3_t dir; } g_lights[8];

static int        g_fog_on;
static float      g_fog_near = 0.0f, g_fog_far = 1.0f;

static KilnHostT3DCounters g_c;

const KilnHostT3DCounters *kiln_host_t3d_counters(void) { return &g_c; }

/* ── lifecycle ────────────────────────────────────────────────────────── */

void t3d_init(T3DInitParams params)
{
    int want = params.matrixStackSize ? params.matrixStackSize : 8;
    assertf(want >= 2, "t3d_init: matrixStackSize must be at least 2 (got %d)", want);
    assertf(want <= MATRIX_STACK,
            "t3d_init: matrixStackSize %d exceeds the host's %d; raise "
            "MATRIX_STACK in plat/host/src/host_t3d.c", want, MATRIX_STACK);
    g_depth = 0;
    memset(&g_c, 0, sizeof g_c);
    memset(g_cache_valid, 0, sizeof g_cache_valid);
}

void t3d_destroy(void) { g_vp = NULL; }

void t3d_frame_start(void)
{
    /* Tiny3D resets the whole render state here, and the rdpq half of that is
     * load-bearing rather than incidental: t3d.c:176 does
     *
     *     rdpq_set_mode_standard();  rdpq_mode_antialias(AA_STANDARD);
     *     rdpq_mode_zbuf(true, true);  rdpq_mode_persp(true); ...
     *
     * so the DEPTH COMPARE is switched on by t3d_frame_start, not by the
     * engine. Nothing in kiln_engine.c calls rdpq_mode_zbuf at all — which
     * reads like a missing depth enable until you find this. Leaving it out
     * here cost a frame in which the far faces of a closed cube drew over the
     * near ones, and it looked exactly like a broken depth test in the
     * rasteriser rather than a missing line in the shim.
     *
     * T3D_FLAG_DEPTH is the RSP's half (compute and emit z); this is the RDP's
     * half (compare and write it). Both are needed, which is the same
     * two-halves shape kiln_scene_begin's own comment describes for fog. */
    rdpq_set_mode_standard();
    rdpq_mode_antialias(1);
    rdpq_mode_zbuf(true, true);

    g_flags = T3D_FLAG_DEPTH | T3D_FLAG_SHADED;
    g_depth = 0;
    memset(g_cache_valid, 0, sizeof g_cache_valid);
    memset(&g_c, 0, sizeof g_c);
}

/* ── viewport ─────────────────────────────────────────────────────────── */

T3DViewport t3d_viewport_create(void)
{
    T3DViewport vp;
    memset(&vp, 0, sizeof vp);
    fm_mat4_identity(&vp.matCamera);
    fm_mat4_identity(&vp.matProj);
    vp.size[0] = kiln_hostfb_w();
    vp.size[1] = kiln_hostfb_h();
    vp.guardBandScale = 2;
    vp.fov = 1.0f; vp.near_z = 1.0f; vp.far_z = 1000.0f;
    return vp;
}

void t3d_viewport_attach(T3DViewport *vp)
{
    assertf(vp != NULL, "t3d_viewport_attach: NULL viewport");
    /* Tiny3D halts the VR4300 inside here when eye == look, because the view
     * matrix is degenerate. CLAUDE.md records that as the reason Forge's CAM
     * mode refuses to save an invalid keyframe table. Reproduce it, loudly:
     * the whole value of the camera validator is that this failure is real. */
    if (vp->size[0] == 0 || vp->size[1] == 0) {
        vp->size[0] = kiln_hostfb_w();
        vp->size[1] = kiln_hostfb_h();
    }
    g_vp = vp;
}

void t3d_viewport_set_projection(T3DViewport *vp, float fov, float near, float far)
{
    assertf(vp != NULL, "t3d_viewport_set_projection: NULL viewport");
    assertf(near > 0.0f, "t3d_viewport_set_projection: near must be > 0 (got %f)",
            (double)near);
    assertf(far > near, "t3d_viewport_set_projection: far %f <= near %f",
            (double)far, (double)near);
    vp->fov = fov; vp->near_z = near; vp->far_z = far;

    const float aspect = (float)vp->size[0] / (float)vp->size[1];
    const float f = 1.0f / tanf(fov * 0.5f);
    fm_mat4_t m;
    memset(&m, 0, sizeof m);
    m.m[0][0] = f / aspect;
    m.m[1][1] = f;
    m.m[2][2] = (far + near) / (near - far);
    m.m[2][3] = -1.0f;
    m.m[3][2] = (2.0f * far * near) / (near - far);
    vp->matProj = m;
}

void t3d_viewport_look_at(T3DViewport *vp, const T3DVec3 *eye,
                          const T3DVec3 *target, const T3DVec3 *up)
{
    assertf(vp && eye && target && up, "t3d_viewport_look_at: NULL argument");

    fm_vec3_t f;
    fm_vec3_sub(&f, target, eye);
    const float len = fm_vec3_len(&f);
    /* eye == look is the one that halts the console. It is a hard error here
     * for the same reason: a degenerate basis renders as a garbage frame or a
     * hang, several layers from the keyframe table that caused it. */
    assertf(len > 1e-6f,
            "t3d_viewport_look_at: eye == target (%.3f %.3f %.3f). "
            "The view basis is degenerate; on console this halts the VR4300 "
            "inside t3d_viewport_attach.",
            (double)eye->v[0], (double)eye->v[1], (double)eye->v[2]);
    fm_vec3_norm(&f, &f);

    fm_vec3_t r, u;
    fm_vec3_cross(&r, &f, up);
    const float rlen = fm_vec3_len(&r);
    assertf(rlen > 1e-6f,
            "t3d_viewport_look_at: up is parallel to the view direction");
    fm_vec3_norm(&r, &r);
    fm_vec3_cross(&u, &r, &f);

    fm_mat4_t m;
    fm_mat4_identity(&m);
    m.m[0][0] = r.v[0]; m.m[1][0] = r.v[1]; m.m[2][0] = r.v[2];
    m.m[0][1] = u.v[0]; m.m[1][1] = u.v[1]; m.m[2][1] = u.v[2];
    m.m[0][2] = -f.v[0]; m.m[1][2] = -f.v[1]; m.m[2][2] = -f.v[2];
    m.m[3][0] = -(r.v[0]*eye->v[0] + r.v[1]*eye->v[1] + r.v[2]*eye->v[2]);
    m.m[3][1] = -(u.v[0]*eye->v[0] + u.v[1]*eye->v[1] + u.v[2]*eye->v[2]);
    m.m[3][2] =  (f.v[0]*eye->v[0] + f.v[1]*eye->v[1] + f.v[2]*eye->v[2]);
    vp->matCamera = m;
}

/* ── matrices, quantised on purpose ───────────────────────────────────── */

/* s16.16: 16 bits of integer, 16 of fraction. Tiny3D's T3DVec4FP splits them
 * into separate int16/uint16 arrays for the ucode; the host only needs the
 * VALUE the console would end up with, so it quantises and keeps a float. */
static inline float quantise_1616(float v)
{
    assertf(v > -32768.0f && v < 32768.0f,
            "t3d_mat4_to_fixed: %f does not fit in s16.16. On console this "
            "wraps silently.", (double)v);
    return (float)((int32_t)(nearbyintf(v * 65536.0f))) / 65536.0f;
}

void t3d_mat4_to_fixed(T3DMat4FP *out, const fm_mat4_t *in)
{
    assertf(out && in, "t3d_mat4_to_fixed: NULL");
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out->m.m[r][c] = quantise_1616(in->m[r][c]);
}

void t3d_mat4_to_fixed_3x4(T3DMat4FP *out, const fm_mat4_t *in)
{
    t3d_mat4_to_fixed(out, in);
}

static void mat_mul(fm_mat4_t *out, const fm_mat4_t *a, const fm_mat4_t *b)
{
    fm_mat4_t r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += a->m[i][k] * b->m[k][j];
            r.m[i][j] = s;
        }
    *out = r;
}

void t3d_matrix_push(T3DMat4FP *mat)
{
    assertf(mat != NULL, "t3d_matrix_push: NULL matrix");
    assertf(g_depth < MATRIX_STACK - 1,
            "t3d_matrix_push: stack overflow at depth %d. On console this "
            "overruns the ucode's matrix buffer.", g_depth);
    fm_mat4_t top;
    if (g_depth == 0) top = mat->m;
    else mat_mul(&top, &g_stack[g_depth - 1], &mat->m);
    g_stack[g_depth++] = top;
    if ((uint32_t)g_depth > g_c.matrix_depth_max) g_c.matrix_depth_max = (uint32_t)g_depth;
}

void t3d_matrix_set(T3DMat4FP *mat, bool doMultiply)
{
    if (doMultiply) { t3d_matrix_push(mat); return; }
    assertf(mat != NULL, "t3d_matrix_set: NULL matrix");
    if (g_depth == 0) g_depth = 1;
    g_stack[g_depth - 1] = mat->m;
}

void t3d_matrix_pop(int count)
{
    assertf(count > 0, "t3d_matrix_pop: count %d", count);
    assertf(g_depth >= count, "t3d_matrix_pop(%d) with depth %d — unbalanced "
            "push/pop", count, g_depth);
    g_depth -= count;
}

void t3d_segment_set(int segment, void *ptr) { (void)segment; (void)ptr; }

/* ── state ────────────────────────────────────────────────────────────── */

void t3d_state_set_drawflags(enum T3DDrawFlags flags) { g_flags = flags; }
void t3d_state_set_vertex_fx(T3DVertexFX fx, int16_t a, int16_t b)
{
    (void)a; (void)b;
    assertf(fx == T3D_VERTEX_FX_NONE,
            "t3d_state_set_vertex_fx(%d): the host implements only "
            "T3D_VERTEX_FX_NONE. Spherical UV, cel shading and outlines are "
            "ucode features with no host equivalent yet.", (int)fx);
}

void t3d_screen_clear_color(color_t c) { kiln_hostfb_clear_color(c); }
void t3d_screen_clear_depth(void)      { kiln_hostfb_clear_depth(); }

/* ── lights ───────────────────────────────────────────────────────────── */

void t3d_light_set_ambient(const uint8_t *color)
{
    assertf(color != NULL, "t3d_light_set_ambient: NULL");
    memcpy(g_ambient, color, 4);
}

void t3d_light_set_count(int count)
{
    /* Tiny3D supports 7 directional lights. KILN_SCENE_MAX_LIGHTS is 4, so a
     * ROM cannot reach this — but the shim is the API, not the engine. */
    assertf(count >= 0 && count <= 7,
            "t3d_light_set_count(%d): Tiny3D supports 0..7", count);
    g_light_count = count;
}

void t3d_light_set_directional(int index, const uint8_t *color, const T3DVec3 *dir)
{
    assertf(index >= 0 && index < 8, "t3d_light_set_directional: index %d", index);
    assertf(color && dir, "t3d_light_set_directional: NULL");
    memcpy(g_lights[index].color, color, 4);
    fm_vec3_t d = *dir;
    if (fm_vec3_len(&d) > 1e-6f) fm_vec3_norm(&d, &d);
    g_lights[index].dir = d;
}

/* ── fog ──────────────────────────────────────────────────────────────── */

void t3d_fog_set_enabled(bool enabled) { g_fog_on = enabled; }
void t3d_fog_set_range(float near, float far)
{
    assertf(far > near, "t3d_fog_set_range: far %f <= near %f",
            (double)far, (double)near);
    g_fog_near = near; g_fog_far = far;
}

/* ── vertices ─────────────────────────────────────────────────────────── */

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

uint16_t t3d_vert_pack_normal(const T3DVec3 *normal)
{
    /* Verbatim from Tiny3D (t3d.c:283). The fields are SIGNED two's complement
     * — 5 bits for x, 6 for y, 5 for z — scaled by 15.5/31.5/15.5, NOT an
     * unsigned [0,max] mapping of [-1,1].
     *
     * That distinction is not cosmetic and it is not visible in a render. An
     * unsigned pack paired with its own matching unpack is self-consistent, so
     * a hand-built cube lights perfectly and looks right — and then a real
     * .t3dm, whose normals were packed by the real encoder, unpacks to
     * nonsense. This was caught by loading assets/cube.gltf's converted model
     * and noticing its +Z face carried 0x000f, which only means (0,0,+1) if
     * the fields are signed. */
    assertf(normal != NULL, "t3d_vert_pack_normal: NULL");
    fm_vec3_t n = *normal;
    if (fm_vec3_len(&n) > 1e-6f) fm_vec3_norm(&n, &n);
    const int xi = clampi((int)lrintf(n.v[0] * 15.5f), -16, 15);
    const int yi = clampi((int)lrintf(n.v[1] * 31.5f), -32, 31);
    const int zi = clampi((int)lrintf(n.v[2] * 15.5f), -16, 15);
    return (uint16_t)((((uint16_t)xi & 0x1Fu) << 11)
                    | (((uint16_t)yi & 0x3Fu) << 5)
                    |  ((uint16_t)zi & 0x1Fu));
}

/* Sign-extend an n-bit two's complement field. */
static inline int sext(unsigned v, int bits)
{
    const unsigned sign = 1u << (bits - 1);
    return (v & sign) ? (int)v - (int)(sign << 1) : (int)v;
}

static void unpack_normal(uint16_t p, fm_vec3_t *out)
{
    out->v[0] = (float)sext((p >> 11) & 0x1Fu, 5) / 15.5f;
    out->v[1] = (float)sext((p >> 5)  & 0x3Fu, 6) / 31.5f;
    out->v[2] = (float)sext( p        & 0x1Fu, 5) / 15.5f;
    if (fm_vec3_len(out) > 1e-6f) fm_vec3_norm(out, out);
}

void t3d_vert_load(const T3DVertPacked *vertices, uint32_t offset, uint32_t count)
{
    assertf(vertices != NULL, "t3d_vert_load: NULL vertex array");
    /* The two limits that are silent on console. offset is capped at 68 rather
     * than 69 because vertices arrive in interleaved pairs. */
    assertf(offset <= T3D_VERTEX_CACHE - 2,
            "t3d_vert_load: offset %u exceeds %d. On console the DMA wraps and "
            "corrupts geometry with no diagnostic.", offset, T3D_VERTEX_CACHE - 2);
    assertf(count >= 1 && count <= T3D_VERTEX_CACHE,
            "t3d_vert_load: count %u outside 1..%d", count, T3D_VERTEX_CACHE);
    assertf(offset + count <= T3D_VERTEX_CACHE,
            "t3d_vert_load: offset %u + count %u overruns the %d-entry vertex "
            "cache. kiln_voxmesh batches 68 for exactly this reason.",
            offset, count, T3D_VERTEX_CACHE);

    assertf(g_vp != NULL, "t3d_vert_load before t3d_viewport_attach: the "
            "transform happens HERE, not at draw time");
    g_c.vert_loads++;
    for (uint32_t i = 0; i < count; i++) {
        const T3DVertPacked *p = &vertices[(offset + i) / 2];
        const int second = ((offset + i) & 1) != 0;
        const int16_t *pos = second ? p->posB : p->posA;
        const int16_t *st  = second ? p->stB  : p->stA;
        const fm_vec3_t mp = {{ (float)pos[0], (float)pos[1], (float)pos[2] }};
        fm_vec3_t nrm;
        unpack_normal(second ? p->normB : p->normA, &nrm);
        transform_vertex(&mp, &nrm, second ? p->rgbaB : p->rgbaA,
                         (float)st[0] / 32.0f,   /* s10.5 pixel coords */
                         (float)st[1] / 32.0f,
                         &g_cache[offset + i]);
        g_cache_valid[offset + i] = 1;
        g_c.verts++;
    }
}

void *t3d_vertbuffer_get_pos(T3DVertPacked *vert, uint32_t idx)
{
    assertf(vert != NULL, "t3d_vertbuffer_get_pos: NULL");
    return (idx & 1) ? (void *)vert[idx / 2].posB : (void *)vert[idx / 2].posA;
}

/* ── triangles ────────────────────────────────────────────────────────── */

/* Lighting, per vertex, exactly where Tiny3D does it: at load time, in the
 * space the matrix has already put the normal into. */
static void shade(const fm_vec3_t *nrm, uint32_t rgba, Out *o)
{
    float r = (float)((rgba >> 24) & 0xFF) / 255.0f;
    float g = (float)((rgba >> 16) & 0xFF) / 255.0f;
    float b = (float)((rgba >> 8) & 0xFF) / 255.0f;
    o->a = (uint8_t)(rgba & 0xFF);

    if (!(g_flags & T3D_FLAG_NO_LIGHT)) {
        float lr = g_ambient[0] / 255.0f, lg = g_ambient[1] / 255.0f,
              lb = g_ambient[2] / 255.0f;
        for (int i = 0; i < g_light_count; i++) {
            /* Tiny3D's light direction points FROM the surface TOWARD the
             * source, so the lambert term is +dot(N, dir): rsp_tiny3d.rspl
             * multiplies the transformed normal by the direction and adds the
             * products straight into the light colour ("usually we want
             * dot(normal, lightDir) * lightColor"), and its examples light
             * from above with a positive y. This read -dot for a long time,
             * which lit every host render from the opposite side: tops and
             * floors that are lit on console came out dark here and the
             * reverse, so content tuned on the host went dark on hardware. */
            float d = nrm->v[0] * g_lights[i].dir.v[0]
                    + nrm->v[1] * g_lights[i].dir.v[1]
                    + nrm->v[2] * g_lights[i].dir.v[2];
            if (d < 0.0f) d = 0.0f;
            lr += d * g_lights[i].color[0] / 255.0f;
            lg += d * g_lights[i].color[1] / 255.0f;
            lb += d * g_lights[i].color[2] / 255.0f;
        }
        r *= lr; g *= lg; b *= lb;
    }
    o->r = (uint8_t)(fminf(r, 1.0f) * 255.0f + 0.5f);
    o->g = (uint8_t)(fminf(g, 1.0f) * 255.0f + 0.5f);
    o->b = (uint8_t)(fminf(b, 1.0f) * 255.0f + 0.5f);
}

/* Model space -> clip space, through the matrix stack top then the attached
 * viewport, plus lighting and fog. Both matrices are column-major (fm_mat4_t's
 * own doc says so), so a row of the product reads m[col][row]. */
static void transform_vertex(const fm_vec3_t *pos, const fm_vec3_t *nrm,
                             uint32_t rgba, float s, float tt, Out *o)
{
    fm_vec3_t p = *pos, n = *nrm;
    if (g_depth > 0) {
        const fm_mat4_t *m = &g_stack[g_depth - 1];
        const fm_vec3_t q = {{
            m->m[0][0]*p.v[0] + m->m[1][0]*p.v[1] + m->m[2][0]*p.v[2] + m->m[3][0],
            m->m[0][1]*p.v[0] + m->m[1][1]*p.v[1] + m->m[2][1]*p.v[2] + m->m[3][1],
            m->m[0][2]*p.v[0] + m->m[1][2]*p.v[1] + m->m[2][2]*p.v[2] + m->m[3][2] }};
        /* The normal takes the rotation only — no translation. Non-uniform
         * scale would want the inverse transpose; bone matrices here are rigid
         * plus uniform scale, where the 3x3 is enough. */
        const fm_vec3_t nn = {{
            m->m[0][0]*n.v[0] + m->m[1][0]*n.v[1] + m->m[2][0]*n.v[2],
            m->m[0][1]*n.v[0] + m->m[1][1]*n.v[1] + m->m[2][1]*n.v[2],
            m->m[0][2]*n.v[0] + m->m[1][2]*n.v[1] + m->m[2][2]*n.v[2] }};
        p = q; n = nn;
        if (fm_vec3_len(&n) > 1e-6f) fm_vec3_norm(&n, &n);
    }

    const fm_mat4_t *cv = &g_vp->matCamera;
    const float ex = cv->m[0][0]*p.v[0] + cv->m[1][0]*p.v[1] + cv->m[2][0]*p.v[2] + cv->m[3][0];
    const float ey = cv->m[0][1]*p.v[0] + cv->m[1][1]*p.v[1] + cv->m[2][1]*p.v[2] + cv->m[3][1];
    const float ez = cv->m[0][2]*p.v[0] + cv->m[1][2]*p.v[1] + cv->m[2][2]*p.v[2] + cv->m[3][2];

    const fm_mat4_t *pr = &g_vp->matProj;
    o->x = pr->m[0][0]*ex;
    o->y = pr->m[1][1]*ey;
    o->z = pr->m[2][2]*ez + pr->m[3][2];
    o->w = -ez;                 /* right-handed: view -Z is forward */

    shade(&n, rgba, o);

    if (g_fog_on) {
        const float d = -ez;
        float f = (d - g_fog_near) / (g_fog_far - g_fog_near);
        f = fminf(fmaxf(f, 0.0f), 1.0f);
        const color_t fc = kiln_hostfb_fog_color();
        o->r = (uint8_t)(o->r + (fc.r - o->r) * f);
        o->g = (uint8_t)(o->g + (fc.g - o->g) * f);
        o->b = (uint8_t)(o->b + (fc.b - o->b) * f);
    }

    const float iw = (o->w > 1e-6f) ? (1.0f / o->w) : 0.0f;
    o->sow = s * iw;
    o->tow = tt * iw;
}

static inline float edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

void t3d_tri_draw(uint32_t i0, uint32_t i1, uint32_t i2)
{
    assertf(kiln_hostfb_attached(), "t3d_tri_draw with nothing attached");
    assertf(g_vp != NULL, "t3d_tri_draw before t3d_viewport_attach");
    const uint32_t idx[3] = { i0, i1, i2 };
    for (int k = 0; k < 3; k++) {
        assertf(idx[k] < T3D_VERTEX_CACHE,
                "t3d_tri_draw: index %u is outside the %d-entry vertex cache",
                idx[k], T3D_VERTEX_CACHE);
        assertf(g_cache_valid[idx[k]],
                "t3d_tri_draw: vertex %u was never loaded. On console this "
                "draws whatever the previous t3d_vert_load left there.", idx[k]);
    }
    g_c.tris_submitted++;

    /* Already transformed, at load time. */
    const Out o[3] = { g_cache[idx[0]], g_cache[idx[1]], g_cache[idx[2]] };

    /* No near-plane clipping: reject the whole triangle if any vertex is at or
     * behind the eye. Tiny3D CLIPS by default (guard band) and only rejects
     * when useRejection is set, so a triangle straddling the near plane
     * disappears here and would be trimmed there. Stated rather than hidden —
     * it shows up as geometry vanishing at the very edge of the frustum, and
     * fixing it means a real clipper. */
    for (int k = 0; k < 3; k++) {
        if (o[k].w <= 1e-4f) { g_c.tris_clipped++; return; }
    }

    const int W = kiln_hostfb_w(), H = kiln_hostfb_h();
    float sx[3], sy[3], sz[3];
    for (int k = 0; k < 3; k++) {
        const float iw = 1.0f / o[k].w;
        sx[k] = ( o[k].x * iw * 0.5f + 0.5f) * (float)W;
        sy[k] = (-o[k].y * iw * 0.5f + 0.5f) * (float)H;
        sz[k] = o[k].z * iw * 0.5f + 0.5f;
    }

    const float area = edge(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2]);
    if (area == 0.0f) { g_c.tris_culled++; return; }

    /* Screen Y points down, which inverts the sense of the cross product
     * relative to world space: a triangle wound counter-clockwise when seen
     * from outside the surface gives a POSITIVE signed area here. */
    const int front = area > 0.0f;
    if (((g_flags & T3D_FLAG_CULL_BACK)  && !front) ||
        ((g_flags & T3D_FLAG_CULL_FRONT) &&  front)) { g_c.tris_culled++; return; }

    int minx = (int)floorf(fminf(sx[0], fminf(sx[1], sx[2])));
    int maxx = (int)ceilf (fmaxf(sx[0], fmaxf(sx[1], sx[2])));
    int miny = (int)floorf(fminf(sy[0], fminf(sy[1], sy[2])));
    int maxy = (int)ceilf (fmaxf(sy[0], fmaxf(sy[1], sy[2])));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > W) maxx = W;
    if (maxy > H) maxy = H;
    if (minx >= maxx || miny >= maxy) { g_c.tris_culled++; return; }

    const int ztest  = (g_flags & T3D_FLAG_DEPTH) && kiln_hostfb_ztest();
    const int zwrite = (g_flags & T3D_FLAG_DEPTH) && kiln_hostfb_zwrite();

    /* Barycentrics without reordering the vertices. The three edge functions
     * carry the sign of the total area, so flipping all of them by that sign
     * makes "inside" mean all-non-negative for either winding. Reordering
     * instead is where this went wrong first: it is easy to normalise the area
     * and forget that the edge functions were not normalised with it, and the
     * result is every pixel rejected and a silently empty frame. */
    const float sgn = (area > 0.0f) ? 1.0f : -1.0f;
    const float inv = 1.0f / fabsf(area);
    int drew = 0;

    for (int y = miny; y < maxy; y++) {
        for (int x = minx; x < maxx; x++) {
            const float px = (float)x + 0.5f, py = (float)y + 0.5f;
            const float e0 = edge(sx[1], sy[1], sx[2], sy[2], px, py) * sgn;
            const float e1 = edge(sx[2], sy[2], sx[0], sy[0], px, py) * sgn;
            const float e2 = edge(sx[0], sy[0], sx[1], sy[1], px, py) * sgn;
            if (e0 < 0.0f || e1 < 0.0f || e2 < 0.0f) continue;
            const float w0 = e0 * inv, w1 = e1 * inv, w2 = e2 * inv;

            float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
            if (z < 0.0f) z = 0.0f;
            if (z > 1.0f) z = 1.0f;

            /* ── the texel, and whether it survives the combiner ──
             * T3D_FLAG_TEXTURED makes the RSP emit texture coordinates. It is
             * the COMBINER that decides whether the sampled texel reaches the
             * framebuffer, and with RDPQ_COMBINER_SHADE — which is what
             * kiln_scene_begin sets every frame — it does not. Reproduced
             * rather than smoothed over: "uploaded but never sampled" is a
             * real state, and a host that sampled anyway would hide it. */
            color_t tex = { 255, 255, 255, 255 };
            int have_tex = 0;
            if (g_flags & T3D_FLAG_TEXTURED) {
                const float iw = w0*(1.0f/o[0].w) + w1*(1.0f/o[1].w)
                               + w2*(1.0f/o[2].w);
                if (iw > 1e-9f) {
                    const float ss = (w0*o[0].sow + w1*o[1].sow + w2*o[2].sow) / iw;
                    const float tt = (w0*o[0].tow + w1*o[1].tow + w2*o[2].tow) / iw;
                    have_tex = kiln_hosttex_sample(0, ss, tt, &tex);
                }
            }

            color_t col;
            if (g_flags & T3D_FLAG_SHADED) {
                col.r = (uint8_t)(w0*o[0].r + w1*o[1].r + w2*o[2].r);
                col.g = (uint8_t)(w0*o[0].g + w1*o[1].g + w2*o[2].g);
                col.b = (uint8_t)(w0*o[0].b + w1*o[1].b + w2*o[2].b);
                col.a = (uint8_t)(w0*o[0].a + w1*o[1].a + w2*o[2].a);
            } else {
                col = (color_t){ o[0].r, o[0].g, o[0].b, o[0].a };
            }

            const rdpq_combiner_t comb = kiln_hostfb_combiner();
            if (have_tex) {
                if (comb == RDPQ_COMBINER_TEX) {
                    col = tex;
                } else if (comb == RDPQ_COMBINER_TEX_SHADE) {
                    col.r = (uint8_t)((col.r * tex.r + 127) / 255);
                    col.g = (uint8_t)((col.g * tex.g + 127) / 255);
                    col.b = (uint8_t)((col.b * tex.b + 127) / 255);
                    col.a = (uint8_t)((col.a * tex.a + 127) / 255);
                } else if (comb == RDPQ_COMBINER_TEX_FLAT) {
                    assertf(0, "RDPQ_COMBINER_TEX_FLAT is not implemented in "
                               "the host 3D pass; add it to host_t3d.c rather "
                               "than letting it render as something else.");
                }
                /* SHADE and FLAT: the texel is discarded, exactly as on
                 * console. Counted so it is at least visible in the numbers. */
                else g_c.texels_discarded++;
            }

            kiln_hostfb_put_z(x, y, (uint16_t)(z * 65535.0f), col, ztest, zwrite);
            drew = 1;
        }
    }
    if (drew) g_c.tris_drawn++;
}

void t3d_tri_sync(void) { }

/* ── strips and sequences ─────────────────────────────────────────────
 * gltf_to_t3d emits STRIPS, not indexed triangles — the first model checked
 * against this reader, a cube, has numIndices 0 and numStripIndices[0] = 24.
 * So a backend that only handled t3d_tri_draw would load every model in this
 * repo and draw nothing.
 *
 * The encoding is Tiny3D's documented one (t3d.h above t3d_indexbuffer_convert):
 * the first three values are a triangle, each value after that extends the
 * strip by one triangle with the winding FLIPPED, and an index with bit 15 set
 * restarts the strip — that value and the two following form a fresh triangle.
 *
 * The host reads the RAW file indices. Tiny3D rewrites them in place into DMEM
 * pointers, which is why t3d_indexbuffer_convert exists; there is no DMEM
 * here, so host_t3dmodel.c deliberately does not call it and this function
 * interprets plain local indices. */
void t3d_tri_draw_strip(int16_t *indexBuff, int count)
{
    assertf(indexBuff != NULL, "t3d_tri_draw_strip: NULL index buffer");
    assertf(count >= 3, "t3d_tri_draw_strip: %d indices cannot form a triangle",
            count);

    uint32_t a = 0, b = 0, c = 0;
    int have = 0, flip = 0;

    for (int i = 0; i < count; i++) {
        const uint16_t raw = (uint16_t)indexBuff[i];
        const int restart = (raw & 0x8000u) != 0;
        const uint32_t idx = raw & 0x7FFFu;
        assertf(idx < T3D_VERTEX_CACHE,
                "t3d_tri_draw_strip: index %u at position %d is outside the "
                "%d-entry vertex cache", idx, i, T3D_VERTEX_CACHE);

        if (restart || have < 3) {
            if (restart) have = 0;
            if (have == 0)      { a = idx; have = 1; flip = 0; continue; }
            else if (have == 1) { b = idx; have = 2; continue; }
            else                { c = idx; have = 3; t3d_tri_draw(a, b, c); continue; }
        }
        /* Extend: drop the oldest vertex and alternate the winding, which is
         * the universal strip convention and what "winding order flipped"
         * means. Nothing in this engine sets a cull flag, so the flip is
         * currently unobservable — but getting it wrong would become visible
         * the moment one is added, which is a bad time to find out. */
        a = b; b = c; c = idx;
        flip = !flip;
        if (flip) t3d_tri_draw(b, a, c);
        else      t3d_tri_draw(a, b, c);
    }
}

void t3d_tri_draw_strip_and_sync(int16_t *indexBuff, int count)
{
    t3d_tri_draw_strip(indexBuff, count);
    t3d_tri_sync();
}

void t3d_tri_draw_unindexed(int base, int count)
{
    /* Sequential triangles straight out of the vertex cache. */
    assertf(base >= 0 && count >= 0, "t3d_tri_draw_unindexed: base %d count %d",
            base, count);
    assertf(base + count * 3 <= T3D_VERTEX_CACHE,
            "t3d_tri_draw_unindexed: %d triangles from %d overruns the "
            "%d-entry vertex cache", count, base, T3D_VERTEX_CACHE);
    for (int i = 0; i < count; i++) {
        const uint32_t v = (uint32_t)(base + i * 3);
        t3d_tri_draw(v, v + 1, v + 2);
    }
}

void t3d_indexbuffer_convert(int16_t indices[], int count)
{
    /* Upstream rewrites local indices into DMEM pointers for the ucode to DMA.
     * The host reads raw indices, so converting would corrupt them. Kept as a
     * no-op rather than removed because the symbol is part of the API and a
     * caller doing its own strip submission still calls it. */
    (void)indices; (void)count;
}
