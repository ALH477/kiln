/* SPDX-License-Identifier: MIT
 *
 * Drives the REAL frame bracket — kiln_frame_begin / kiln_scene_begin /
 * kiln_gui_begin / kiln_frame_end — on the host, with the real
 * engine/src/kiln/kiln_engine.c and kiln_gui.c, and captures the result.
 *
 * That bracket is the engine's one state transition per frame and CLAUDE.md
 * calls it the seam that makes drawing HUD inside the 3D pass an obvious
 * mistake. Until now nothing outside a ROM had ever executed it.
 *
 * The scene is built to exercise the things a host 3D pass can actually be
 * wrong about:
 *
 *   depth       two cubes overlap in screen space at different distances, so a
 *               broken compare shows as the far one winning.
 *   lighting    the ambient term plus two directional lights, on a cube whose
 *               faces have known normals — a wrong normal unpack or a sign
 *               error on the dot product makes the shading flat or inverted.
 *   matrices    each cube goes through a KilnTransform, i.e. through
 *               t3d_mat4_to_fixed's s16.16 quantisation and t3d_matrix_push.
 *   fog         enabled with a range that puts the far cube inside it.
 *   the seam    a HUD is drawn afterwards, and must land ON TOP with no depth
 *               interaction at all.
 */
#include <kiln_engine.h>
#include <kiln_gui.h>
#include <kiln_host.h>
#include <t3d/t3d.h>
#include <libdragon.h>

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ── a cube, packed exactly as kiln_voxmesh packs geometry ──
 * Two vertices per T3DVertPacked, positions as int16 (the integer part of the
 * ucode's s16.16), one packed normal per vertex, RGBA8 per vertex. Building it
 * by hand rather than loading a model is the point: it is the same submission
 * path kiln_map_draw and kiln_voxmesh use, and it needs no .t3dm parser. */
#define S 40   /* half-extent in world units */

typedef struct { int nx, ny, nz; uint8_t r, g, b; } Face;
static const Face FACES[6] = {
    { +1,  0,  0, 0xE0, 0x40, 0x40 },   /* +X red    */
    { -1,  0,  0, 0x40, 0x40, 0xE0 },   /* -X blue   */
    {  0, +1,  0, 0xF0, 0xF0, 0xF0 },   /* +Y white  */
    {  0, -1,  0, 0x30, 0x30, 0x38 },   /* -Y dark   */
    {  0,  0, +1, 0x40, 0xE0, 0x60 },   /* +Z green  */
    {  0,  0, -1, 0xE0, 0xC0, 0x30 },   /* -Z yellow */
};

/* 24 vertices = 12 T3DVertPacked. Under the 70-entry cache, so one load. */
static T3DVertPacked g_cube[12];

static void corner(int f, int c, int16_t out[3])
{
    const Face *F = &FACES[f];
    /* Two in-plane axes per face, chosen right-handed against the normal
     * (Y x Z = X, Z x X = Y, X x Y = Z) so corners 0..3 wind counter-clockwise
     * as seen from OUTSIDE. */
    int ax[3] = { 0, 0, 0 }, ay[3] = { 0, 0, 0 };
    if      (F->nx) { ax[1] = 1; ay[2] = 1; }
    else if (F->ny) { ax[2] = 1; ay[0] = 1; }
    else            { ax[0] = 1; ay[1] = 1; }

    /* A face on the negative side sits at -S with the same in-plane basis, so
     * its winding is mirrored. Flip one axis to put it back — otherwise three
     * of the six faces are inside-out and back-face culling silently removes
     * the wrong half of the cube. */
    const int flip = (F->nx + F->ny + F->nz) < 0;
    const int sx = (c == 0 || c == 3) ? -1 : +1;
    int sy = (c < 2) ? -1 : +1;
    if (flip) sy = -sy;

    for (int k = 0; k < 3; k++) {
        const int n = (k == 0 ? F->nx : k == 1 ? F->ny : F->nz);
        out[k] = (int16_t)(n * S + ax[k] * sx * S + ay[k] * sy * S);
    }
}

static void build_cube(void)
{
    for (int f = 0; f < 6; f++) {
        const Face *F = &FACES[f];
        T3DVec3 nrm = {{ (float)F->nx, (float)F->ny, (float)F->nz }};
        const uint16_t pn = t3d_vert_pack_normal(&nrm);
        const uint32_t rgba = ((uint32_t)F->r << 24) | ((uint32_t)F->g << 16)
                            | ((uint32_t)F->b << 8) | 0xFF;
        for (int c = 0; c < 4; c++) {
            const int vi = f * 4 + c;
            T3DVertPacked *p = &g_cube[vi / 2];
            int16_t pos[3];
            corner(f, c, pos);
            if (vi & 1) {
                memcpy(p->posB, pos, sizeof pos); p->normB = pn; p->rgbaB = rgba;
                p->stB[0] = p->stB[1] = 0;
            } else {
                memcpy(p->posA, pos, sizeof pos); p->normA = pn; p->rgbaA = rgba;
                p->stA[0] = p->stA[1] = 0;
            }
        }
    }
}

static void draw_cube(void)
{
    t3d_vert_load(g_cube, 0, 24);
    for (int f = 0; f < 6; f++) {
        const uint32_t b = (uint32_t)f * 4;
        t3d_tri_draw(b + 0, b + 1, b + 2);
        t3d_tri_draw(b + 0, b + 2, b + 3);
    }
    t3d_tri_sync();
}

int main(int argc, char **argv)
{
    const char *png = argc > 1 ? argv[1] : "scene.png";
    const char *manifest = argc > 2 ? argv[2] : "scene.txt";

    kiln_engine_init(RESOLUTION_320x240);
    kiln_gui_init();
    build_cube();

    KilnScene scene;
    kiln_scene_init(&scene);
    scene.cam_pos    = (fm_vec3_t){{ 130.0f,  95.0f, 210.0f }};
    scene.cam_target = (fm_vec3_t){{   0.0f,   0.0f,   0.0f }};
    scene.fov_deg    = 55.0f;
    scene.near_z     = 10.0f;
    scene.far_z      = 900.0f;
    scene.clear_color = RGBA32(0x0A, 0x0C, 0x14, 0xFF);

    /* Ambient plus two directional lights from opposite sides, so every face
     * of the cube gets a different value and a flat result means the normals
     * are not reaching the shader. */
    scene.ambient[0] = scene.ambient[1] = scene.ambient[2] = 0x40;
    scene.ambient[3] = 0xFF;
    scene.light_color[0] = 0xFF; scene.light_color[1] = 0xF0;
    scene.light_color[2] = 0xC0; scene.light_color[3] = 0xFF;
    scene.light_dir = (fm_vec3_t){{ 0.55f, 0.70f, 0.45f }};
    scene.lights[0].color[0] = 0x30; scene.lights[0].color[1] = 0x50;
    scene.lights[0].color[2] = 0x90; scene.lights[0].color[3] = 0xFF;
    scene.lights[0].dir = (fm_vec3_t){{ -0.80f, -0.20f, -0.55f }};
    scene.light_count = 2;

    kiln_scene_set_fog(&scene, RGBA32(0x0A, 0x0C, 0x14, 0xFF), 260.0f, 620.0f);

    KilnTransform near_t, far_t;
    kiln_transform_init(&near_t);
    kiln_transform_init(&far_t);
    near_t.pos = (fm_vec3_t){{ -30.0f, 0.0f,  40.0f }};
    near_t.rot_axis = (fm_vec3_t){{ 0.3f, 1.0f, 0.1f }};
    near_t.rot_angle = 0.6f;
    /* Behind and to the right, overlapping in screen space: the depth test is
     * what decides which one you see in the overlap. */
    far_t.pos = (fm_vec3_t){{ 55.0f, 10.0f, -230.0f }};
    far_t.rot_axis = (fm_vec3_t){{ 0.0f, 1.0f, 0.0f }};
    far_t.rot_angle = 0.9f;
    far_t.scale = (fm_vec3_t){{ 1.6f, 1.6f, 1.6f }};

    kiln_scene_update(&scene);

    kiln_frame_begin();
      kiln_scene_begin(&scene);
        kiln_transform_push(&far_t);  draw_cube(); kiln_transform_pop();
        kiln_transform_push(&near_t); draw_cube(); kiln_transform_pop();
      kiln_gui_begin();
        kiln_gui_rect(0, 0, 320, 12, RGBA32(0x10, 0x12, 0x18, 0xFF));
        kiln_gui_text(6, 9, RGBA32(0x00, 0xF5, 0xD4, 0xFF), "KILN SCENE");
        kiln_gui_panel(8, 20, 116, 44, RGBA32(0x10, 0x12, 0x18, 0xFF),
                       RGBA32(0x00, 0xF5, 0xD4, 0xFF));
        kiln_gui_text(14, 34, RGBA32(0xFF,0xFF,0xFF,0xFF), "lights %d", scene.light_count);
        kiln_gui_text(14, 48, RGBA32(0xFF,0xFF,0xFF,0xFF), "fog on");
        /* Straight through the middle of both cubes. It must be visible over
         * them: the HUD has no depth interaction by construction. */
        kiln_gui_line(0, 150, 319, 120, 1, RGBA32(0xFF, 0x30, 0x60, 0xFF));
      kiln_gui_end();
    kiln_frame_end();

    const KilnHostT3DCounters *t = kiln_host_t3d_counters();
    printf("  3D: vert_loads %u verts %u submitted %u drawn %u culled %u "
           "clipped %u matdepth %u\n",
           t->vert_loads, t->verts, t->tris_submitted, t->tris_drawn,
           t->tris_culled, t->tris_clipped, t->matrix_depth_max);

    /* Two cubes, 12 triangles each. */
    CHECK(t->tris_submitted == 24, "expected 24 triangles submitted, got %u",
          t->tris_submitted);
    /* All 24, because kiln_scene_begin sets T3D_FLAG_SHADED | T3D_FLAG_DEPTH
     * and NOT either cull flag — the engine resolves closed geometry with the
     * depth buffer rather than by discarding back faces. Worth pinning: on a
     * fill-rate-bound console that is roughly twice the shaded pixels for any
     * closed mesh, and it is the kind of decision that is invisible until
     * something counts it. If a cull flag is ever added, this fails and the
     * new number is the measurement of what it bought. */
    CHECK(t->tris_drawn == 24,
          "%u triangles drawn; expected all 24 (the engine sets no cull flag)",
          t->tris_drawn);
    CHECK(t->tris_clipped == 0, "%u triangles were near-plane rejected",
          t->tris_clipped);
    CHECK(t->vert_loads == 2, "expected one vertex load per cube, got %u",
          t->vert_loads);
    CHECK(t->matrix_depth_max == 1, "matrix depth peaked at %u, expected 1",
          t->matrix_depth_max);
    /* Depth must actually be resolving something: the two cubes overlap in
     * screen space, so if the far one were drawn on top the frame would be
     * wrong in a way the reference PNG catches but the counters would not. */
    CHECK(t->tris_culled == 0, "%u triangles culled; none should be",
          t->tris_culled);

    kiln_host_stats(stdout, 8);

    CHECK(kiln_host_capture(png) == 0, "could not write %s", png);
    CHECK(kiln_host_text_manifest(manifest) == 0, "could not write %s", manifest);

    if (fails) { printf("\nFAILED (%d)\n", fails); return 1; }
    printf("the real frame bracket ran on the host: 3D pass, seam, 2D pass\n");
    return 0;
}
