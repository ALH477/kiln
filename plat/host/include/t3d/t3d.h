/* SPDX-License-Identifier: MIT
 *
 * plat/host/include/t3d/t3d.h — the host's <t3d/t3d.h>.
 *
 * Tiny3D's real header is a thin wrapper over RSP microcode: t3d_vert_load is
 * a DMA into a 70-entry vertex cache, t3d_tri_draw is a command in an RSPQ
 * block, and the matrices are s16.16 because that is what the ucode reads. So
 * this cannot be "the real header with the asm removed" the way
 * nix/host-math.nix manages for fgeom.h — it is a reimplementation of the API,
 * and the rule is that every structure the ENGINE builds is copied byte for
 * byte, while everything only passed through is the host's own business.
 *
 *   copied verbatim    T3DVertPacked, the draw flags, the vertex-FX enum.
 *                      kiln_voxmesh and kiln_map pack vertices into these by
 *                      hand — offsets, widths and the two-vertices-per-struct
 *                      interleave are load-bearing, and a 32-byte struct that
 *                      merely had the right fields would compile and mean
 *                      something else.
 *   host's own         T3DViewport, T3DMat4FP. The engine never reads a member
 *                      of either (verified: no member access anywhere in
 *                      engine/src/kiln, and kiln_scene_project computes its
 *                      own view basis from KilnScene's camera fields rather
 *                      than from the viewport). They are storage, so they hold
 *                      what this backend needs.
 *
 * ── What is deliberately loud rather than absent ──────────────────────
 * The model, skeleton and animation entry points are DECLARED here and abort
 * with a message if called. That is on purpose: it lets the modules that
 * reference them compile and link, so they get -Werror and appear honestly in
 * HOST_MODULES, while making a call site fail in a way nobody can mistake for
 * working. A silent no-op would draw nothing and look exactly like a camera
 * pointed the wrong way — which CLAUDE.md records as having cost this project
 * a full pass of camera retuning once already.
 */
#ifndef KILN_HOST_T3D_H
#define KILN_HOST_T3D_H

#include <stdint.h>
#include <stdbool.h>
#include <libdragon.h>
#include <t3d/t3dmath.h>

/* ── vertices: verbatim from Tiny3D ───────────────────────────────────── */
typedef struct {
    /* 0x00 */ int16_t  posA[3];  /* s16, the integer part of an s16.16      */
    /* 0x06 */ uint16_t normA;    /* 5,6,5 packed normal                     */
    /* 0x08 */ int16_t  posB[3];
    /* 0x0E */ uint16_t normB;
    /* 0x10 */ uint32_t rgbaA;    /* RGBA8                                   */
    /* 0x14 */ uint32_t rgbaB;
    /* 0x18 */ int16_t  stA[2];   /* UV, s10.5 pixel coords                  */
    /* 0x1C */ int16_t  stB[2];
} __attribute__((aligned(8))) T3DVertPacked;

_Static_assert(sizeof(T3DVertPacked) == 0x20, "T3DVertPacked has wrong size");

enum T3DDrawFlags {
    T3D_FLAG_DEPTH      = 1 << 0,
    T3D_FLAG_TEXTURED   = 1 << 1,
    T3D_FLAG_SHADED     = 1 << 2,
    T3D_FLAG_CULL_FRONT = 1 << 3,
    T3D_FLAG_CULL_BACK  = 1 << 4,
    T3D_FLAG_NO_LIGHT   = 1 << 16,
};

enum T3DSegment { T3D_SEGMENT_1 = 1, T3D_SEGMENT_2, T3D_SEGMENT_3, T3D_SEGMENT_4 };

/* Tagged, because kiln_vanim casts to `enum T3DVertexFX` and an
 * anonymous enum leaves that spelling incomplete. */
typedef enum T3DVertexFX { T3D_VERTEX_FX_NONE = 0, T3D_VERTEX_FX_SPHERICAL_UV = 1,
               T3D_VERTEX_FX_CELSHADE_COLOR = 2, T3D_VERTEX_FX_OUTLINE = 3 } T3DVertexFX;

/* ── the RSP vertex cache, which is the interesting constraint ─────────
 * 70 entries. Exceeding it silently corrupts geometry on console — the DMA
 * simply wraps — so the host asserts instead. kiln_voxmesh batches 68 for
 * exactly this reason (68 and not 70 because a quad is four vertices and no
 * quad may straddle a load), and this is what proves that arithmetic. */
#define T3D_VERTEX_CACHE 70

/* ── host-owned storage ───────────────────────────────────────────────── */
typedef struct { fm_mat4_t m; } T3DMat4FP;

typedef struct {
    fm_mat4_t matCamera;
    fm_mat4_t matProj;
    int32_t   offset[2];
    int32_t   size[2];
    int       guardBandScale;
    int       useRejection;
    /* Retained because a projection is not recoverable from the matrix
     * without a decompose, and the rasteriser wants the near/far plane. */
    float     fov, near_z, far_z;
} T3DViewport;

typedef struct { int matrixStackSize; } T3DInitParams;
typedef struct { uint32_t trisPreCull, trisPostCull; } T3DMetrics;

/* ── lifecycle ────────────────────────────────────────────────────────── */
void t3d_init(T3DInitParams params);
void t3d_destroy(void);
void t3d_frame_start(void);

/* ── viewport ─────────────────────────────────────────────────────────── */
T3DViewport t3d_viewport_create(void);
void t3d_viewport_attach(T3DViewport *vp);
void t3d_viewport_set_projection(T3DViewport *vp, float fov, float near, float far);
void t3d_viewport_look_at(T3DViewport *vp, const T3DVec3 *eye,
                          const T3DVec3 *target, const T3DVec3 *up);

/* ── matrices ─────────────────────────────────────────────────────────── */
void t3d_mat4_to_fixed(T3DMat4FP *out, const fm_mat4_t *in);
void t3d_mat4_to_fixed_3x4(T3DMat4FP *out, const fm_mat4_t *in);
void t3d_matrix_push(T3DMat4FP *mat);
void t3d_matrix_pop(int count);
void t3d_matrix_set(T3DMat4FP *mat, bool doMultiply);
void t3d_segment_set(int segment, void *ptr);

/* ── state ────────────────────────────────────────────────────────────── */
void t3d_state_set_drawflags(enum T3DDrawFlags flags);
void t3d_state_set_vertex_fx(T3DVertexFX fx, int16_t a, int16_t b);
void t3d_screen_clear_color(color_t c);
void t3d_screen_clear_depth(void);

/* ── lights ───────────────────────────────────────────────────────────── */
void t3d_light_set_ambient(const uint8_t *color);
void t3d_light_set_count(int count);
void t3d_light_set_directional(int index, const uint8_t *color, const T3DVec3 *dir);

/* ── fog ──────────────────────────────────────────────────────────────── */
void t3d_fog_set_enabled(bool enabled);
void t3d_fog_set_range(float near, float far);

/* ── geometry ─────────────────────────────────────────────────────────── */
void     t3d_vert_load(const T3DVertPacked *vertices, uint32_t offset, uint32_t count);
void     t3d_tri_draw(uint32_t v0, uint32_t v1, uint32_t v2);
void     t3d_tri_sync(void);
uint16_t t3d_vert_pack_normal(const T3DVec3 *normal);
void    *t3d_vertbuffer_get_pos(T3DVertPacked *vert, uint32_t idx);

/* ── counters the console cannot report ───────────────────────────────── */
typedef struct {
    uint32_t vert_loads, verts, tris_submitted, tris_drawn;
    uint32_t tris_culled, tris_clipped;
    uint32_t matrix_depth_max;
    /* Texels sampled and then thrown away because the combiner does not use
     * them. A non-zero value means a texture was uploaded, coordinates were
     * emitted, and none of it reached the screen. */
    uint32_t texels_discarded;
} KilnHostT3DCounters;

const KilnHostT3DCounters *kiln_host_t3d_counters(void);

#endif /* KILN_HOST_T3D_H */
