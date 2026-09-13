/* SPDX-License-Identifier: MIT
 *
 * kiln_prim, asserted on the host with the real kiln_prim.c.
 *
 * What a primitive builder gets wrong renders plausibly, which is why this
 * reads the vertices back instead of looking at a picture:
 *
 *   winding     every quad must turn counter-clockwise seen from the side its
 *               normal points to. The engine sets no cull flag today, so a face
 *               wound inside-out draws perfectly — until somebody sets one, and
 *               half of every box vanishes. Checked with a cross product.
 *   normals     each face's packed normal must unpack to its axis, through the
 *               SIGNED 5.6.5 decode (the .t3dm trap in CLAUDE.md).
 *   extents     every vertex must lie on its face's plane and inside the box,
 *               so the `offset` hinge moves the box and does not reshape it.
 *   batching    a 256-cell floor must go out in 17-quad loads — the host's
 *               vertex cache asserts if one ever exceeds 70.
 *   the stage   clear colour equals fog colour and two lights are uploaded.
 */
#include <kiln_engine.h>
#include <kiln_gui.h>
#include <kiln_prim.h>
#include <kiln_host.h>
#include <t3d/t3d.h>
#include <libdragon.h>

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* The last presented frame, via the launcher seam (kiln_host.h). */
static uint8_t g_frame[320 * 240 * 4];
static int g_frame_w, g_frame_h;
static void grab(void *ctx, const void *rgba8, int w, int h)
{
    (void)ctx;
    if (w * h * 4 > (int)sizeof g_frame) return;
    memcpy(g_frame, rgba8, (size_t)(w * h * 4));
    g_frame_w = w; g_frame_h = h;
}

static int sext(unsigned v, int bits)
{
    const unsigned sign = 1u << (bits - 1);
    return (v & sign) ? (int)v - (int)(sign << 1) : (int)v;
}

static void vert(const KilnPrim *p, int vi, fm_vec3_t *pos, fm_vec3_t *nrm, uint32_t *rgba)
{
    const T3DVertPacked *e = &p->verts[vi / 2];
    const int16_t *s = (vi & 1) ? e->posB : e->posA;
    const uint16_t n = (vi & 1) ? e->normB : e->normA;
    pos->v[0] = s[0]; pos->v[1] = s[1]; pos->v[2] = s[2];
    nrm->v[0] = (float)sext((n >> 11) & 0x1Fu, 5) / 15.5f;
    nrm->v[1] = (float)sext((n >> 5)  & 0x3Fu, 6) / 31.5f;
    nrm->v[2] = (float)sext( n        & 0x1Fu, 5) / 15.5f;
    if (rgba) *rgba = (vi & 1) ? e->rgbaB : e->rgbaA;
}

/* Winding + planarity for every quad of a prim. Returns faces seen per axis
 * sign in `axes` (bit 2*axis + (sign<0)). */
static unsigned check_quads(const char *what, const KilnPrim *p)
{
    unsigned axes = 0;
    for (int q = 0; q < p->quad_count; q++) {
        fm_vec3_t P[4], N[4];
        for (int i = 0; i < 4; i++) vert(p, q * 4 + i, &P[i], &N[i], NULL);

        fm_vec3_t e1, e2, cr;
        fm_vec3_sub(&e1, &P[1], &P[0]);
        fm_vec3_sub(&e2, &P[2], &P[0]);
        fm_vec3_cross(&cr, &e1, &e2);
        const float facing = fm_vec3_dot(&cr, &N[0]);
        CHECK(facing > 0.0f,
              "%s quad %d winds clockwise seen from its normal (%.2f %.2f %.2f); "
              "it is inside-out and a back-face cull would delete it",
              what, q, N[0].v[0], N[0].v[1], N[0].v[2]);

        int axis = -1, sign = 0;
        for (int k = 0; k < 3; k++) {
            if (N[0].v[k] > 0.9f)  { axis = k; sign = +1; }
            if (N[0].v[k] < -0.9f) { axis = k; sign = -1; }
        }
        CHECK(axis >= 0, "%s quad %d normal (%.2f %.2f %.2f) is not an axis",
              what, q, N[0].v[0], N[0].v[1], N[0].v[2]);
        if (axis < 0) continue;
        axes |= 1u << (axis * 2 + (sign < 0));
        for (int i = 1; i < 4; i++) {
            CHECK(P[i].v[axis] == P[0].v[axis],
                  "%s quad %d vertex %d is off its plane", what, q, i);
            CHECK(memcmp(&N[i], &N[0], sizeof N[0]) == 0,
                  "%s quad %d vertex %d carries a different normal", what, q, i);
        }
    }
    return axes;
}

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    kiln_host_set_hooks(&(KilnHostHooks){ .present = grab });

    /* ── a box off its origin, as a hinged door is built ── */
    KilnPrim box;
    CHECK(kiln_prim_box(&box, (fm_vec3_t){{ 10, 0, 0 }}, (fm_vec3_t){{ 10, 20, 2 }},
                        0xFF0000FF, 0x00FF00FF, 0x0000FFFF) == 0, "box alloc");
    CHECK(box.quad_count == 6 && box.vert_count == 24,
          "box is %u quads / %u verts, expected 6 / 24", box.quad_count, box.vert_count);
    CHECK(check_quads("box", &box) == 0x3F, "box does not have one face per axis sign");

    for (int vi = 0; vi < box.vert_count; vi++) {
        fm_vec3_t p, n; uint32_t c;
        vert(&box, vi, &p, &n, &c);
        CHECK(p.v[0] >= 0 && p.v[0] <= 20 && p.v[1] >= -20 && p.v[1] <= 20 &&
              p.v[2] >= -2 && p.v[2] <= 2,
              "box vertex %d (%.0f %.0f %.0f) is outside offset+half", vi,
              p.v[0], p.v[1], p.v[2]);
        const uint32_t want = n.v[1] > 0.9f ? 0xFF0000FFu
                            : n.v[1] < -0.9f ? 0x0000FFFFu : 0x00FF00FFu;
        CHECK(c == want, "box vertex %d colour %08x, expected %08x", vi, c, want);
    }

    /* ── a 16x16 floor ── */
    KilnPrim floor_;
    CHECK(kiln_prim_floor(&floor_, 160.0f, 16, 0x909090FF, 0xA0A0A0FF) == 0, "floor alloc");
    CHECK(floor_.quad_count == 256, "floor has %u quads, expected 256", floor_.quad_count);
    CHECK(check_quads("floor", &floor_) == 1u << 2, "floor faces are not all +Y");
    {
        fm_vec3_t p, n; uint32_t c0, c1;
        vert(&floor_, 0, &p, &n, &c0);
        vert(&floor_, 4, &p, &n, &c1);
        CHECK(c0 != c1, "neighbouring floor cells share a colour; it is not a checker");
    }

    /* ── a scene through the real frame bracket ── */
    KilnScene scene;
    kiln_scene_init(&scene);
    kiln_prim_stage(&scene, RGBA32(0x20, 0x30, 0x48, 0xFF), 150.0f, 400.0f);
    CHECK(scene.light_count == 2, "stage uploaded %d lights, expected 2", scene.light_count);
    CHECK(scene.fog_enabled, "stage left fog off with a valid range");
    CHECK(scene.fog_color.r == scene.clear_color.r && scene.fog_color.g == scene.clear_color.g &&
          scene.fog_color.b == scene.clear_color.b,
          "fog colour differs from the clear colour; far geometry will end at an edge");
    scene.cam_pos = (fm_vec3_t){{ 0, 90, -200 }};
    scene.far_z = 600.0f;
    kiln_scene_update(&scene);

    KilnTransform t;
    kiln_transform_init(&t);
    t.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
    t.rot_angle = 0.7f;
    t.pos = (fm_vec3_t){{ 0, 20, 0 }};

    kiln_frame_begin();
      kiln_scene_begin(&scene);
        KilnTransform id; kiln_transform_init(&id);
        kiln_transform_push(&id); kiln_prim_draw(&floor_); kiln_transform_pop();
        kiln_transform_push(&t);  kiln_prim_draw(&box);    kiln_transform_pop();
        kiln_transform_free(&id);
      kiln_gui_begin();
      kiln_gui_end();
    kiln_frame_end();

    const KilnHostT3DCounters *tc = kiln_host_t3d_counters();
    printf("  3D: vert_loads %u verts %u submitted %u drawn %u\n",
           tc->vert_loads, tc->verts, tc->tris_submitted, tc->tris_drawn);
    /* 256 quads at 17 per load is 16 loads (15 full + one of 1), plus the box. */
    CHECK(tc->vert_loads == 16 + 1, "expected 17 vertex loads, got %u", tc->vert_loads);
    CHECK(tc->tris_submitted == 512 + 12, "expected 524 triangles, got %u",
          tc->tris_submitted);
    CHECK(kiln_host_counters()->shaded_px > 0, "the frame shaded no pixels");

    /* A floor under the stage's key light must come out well above ambient.
     * Ambient alone lands a 0x90 grey near 0x1C; lit from overhead it is
     * several times that. This is the assertion that catches the stage's key
     * pointing the wrong way, which leaves every floor black while wall sides
     * look fine — and, with plat/host's lambert sign fixed, it now means the
     * same thing on console. Sampled low in the frame, where only floor is drawn. */
    CHECK(g_frame_w == 320 && g_frame_h == 240, "no frame was presented (%dx%d)",
          g_frame_w, g_frame_h);
    if (g_frame_w == 320) {
        const uint8_t *px = &g_frame[(225 * 320 + 60) * 4];
        const int lum = (px[0] + px[1] + px[2]) / 3;
        printf("  floor pixel (60,225): %d %d %d\n", px[0], px[1], px[2]);
        CHECK(lum > 80, "a floor under the stage's key light is lum %d; it is lit "
              "from below, i.e. the light direction's sign is wrong", lum);
    }

    kiln_transform_free(&t);
    kiln_prim_free(&box);
    kiln_prim_free(&floor_);
    CHECK(box.verts == NULL, "kiln_prim_free left a dangling pointer");

    if (fails) { printf("\nFAILED (%d)\n", fails); return 1; }
    printf("kiln_prim: winding, normals, extents, batching and the stage all hold\n");
    return 0;
}
