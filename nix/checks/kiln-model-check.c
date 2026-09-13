/* SPDX-License-Identifier: MIT
 *
 * Loads a real .t3dm — the same file gltf_to_t3d produces for the ROM — with
 * the host reader, asserts its structure against what the converter reported,
 * and draws it.
 *
 * The structural assertions matter more than usual here because this is the
 * first part of the host backend that reimplements a FILE FORMAT rather than
 * an API (Tiny3D's loader relocates in place, which a 64-bit host cannot do —
 * see host_t3dmodel.c). A misread offset does not fail; it produces geometry.
 *
 * The one that would have caught the most: gltf_to_t3d emits triangle STRIPS,
 * not indexed triangles. A cube comes out as numIndices 0 and
 * numStripIndices[0] = 24 — six groups of four, each group a quad, two
 * triangles each, twelve in total. A reader that only handled t3d_tri_draw
 * would load this file perfectly and draw nothing at all.
 */
#include <t3d/t3dmodel.h>
#include <kiln_engine.h>
#include <kiln_gui.h>
#include <kiln_host.h>
#include <libdragon.h>

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "cube.t3dm";
    const char *png  = argc > 2 ? argv[2] : "model.png";

    kiln_engine_init(RESOLUTION_320x240);
    kiln_gui_init();

    T3DModel *model = t3d_model_load(path);
    CHECK(model != NULL, "t3d_model_load returned NULL");
    if (!model) return 1;

    printf("  magic %.3s v%d  verts %u  indices %u  aabb [%d %d %d]..[%d %d %d]\n",
           model->magic, model->magic[3], model->totalVertCount,
           model->totalIndexCount, model->aabbMin[0], model->aabbMin[1],
           model->aabbMin[2], model->aabbMax[0], model->aabbMax[1],
           model->aabbMax[2]);

    /* assets/cube.gltf is a 64-unit cube built by tools/blender; gltf_to_t3d
     * reports 24 vertices (four per face, unshared because each face has its
     * own normal) and no index buffer. */
    CHECK(memcmp(model->magic, "T3M", 3) == 0, "bad magic");
    CHECK(model->totalVertCount == 24, "totalVertCount %u, expected 24",
          model->totalVertCount);
    CHECK(model->aabbMin[0] == -32 && model->aabbMax[0] == 32,
          "aabb x is %d..%d, expected -32..32", model->aabbMin[0],
          model->aabbMax[0]);

    int objects = 0, parts = 0, strip_indices = 0, indexed = 0;
    T3DModelIter it = t3d_model_iter_create(model, T3D_CHUNK_TYPE_OBJECT);
    while (t3d_model_iter_next(&it)) {
        objects++;
        printf("  object '%s': %u parts, %u tris, material %s\n",
               it.object->name ? it.object->name : "(unnamed)",
               it.object->numParts, it.object->triCount,
               it.object->material ? "yes" : "none");
        for (uint32_t p = 0; p < it.object->numParts; p++) {
            const T3DObjectPart *pt = &it.object->parts[p];
            parts++;
            indexed += pt->numIndices;
            for (int s = 0; s < 4; s++) strip_indices += pt->numStripIndices[s];
            printf("    part%u: load %u at %u, indices %u, strips %u/%u/%u/%u\n",
                   p, pt->vertLoadCount, pt->vertDestOffset, pt->numIndices,
                   pt->numStripIndices[0], pt->numStripIndices[1],
                   pt->numStripIndices[2], pt->numStripIndices[3]);
        }
    }
    CHECK(objects == 1, "%d objects, expected 1", objects);
    CHECK(parts == 1, "%d parts, expected 1", parts);
    /* The finding, pinned: this model is strips, not an index buffer. If
     * gltf_to_t3d ever changes that, the reader's indexed path takes over and
     * this fails — which is the notification you want. */
    CHECK(indexed == 0, "%d indexed vertices; this model should be strips only",
          indexed);
    CHECK(strip_indices == 24, "%d strip indices, expected 24", strip_indices);

    KilnScene scene;
    kiln_scene_init(&scene);
    scene.cam_pos    = (fm_vec3_t){{ 105.0f, 86.0f, 132.0f }};
    scene.cam_target = (fm_vec3_t){{  0.0f,  0.0f,  0.0f }};
    scene.fov_deg    = 50.0f;
    scene.near_z     = 5.0f;
    scene.far_z      = 400.0f;
    scene.clear_color = RGBA32(0x08, 0x0A, 0x10, 0xFF);
    scene.ambient[0] = scene.ambient[1] = scene.ambient[2] = 0x50;
    scene.ambient[3] = 0xFF;
    scene.light_color[0] = 0xFF; scene.light_color[1] = 0xF4;
    scene.light_color[2] = 0xD0; scene.light_color[3] = 0xFF;
    scene.light_dir = (fm_vec3_t){{ 0.5f, 0.75f, 0.4f }};
    scene.light_count = 1;
    kiln_scene_update(&scene);

    KilnTransform xf;
    kiln_transform_init(&xf);
    xf.rot_axis = (fm_vec3_t){{ 0.25f, 1.0f, 0.0f }};
    xf.rot_angle = 0.55f;

    kiln_frame_begin();
      kiln_scene_begin(&scene);
        kiln_transform_push(&xf);
          t3d_model_draw(model);
        kiln_transform_pop();
      kiln_gui_begin();
        kiln_gui_rect(0, 0, 320, 12, RGBA32(0x10, 0x12, 0x18, 0xFF));
        kiln_gui_text(6, 9, RGBA32(0x00, 0xF5, 0xD4, 0xFF), "KILN MODEL");
        kiln_gui_text(6, 232, RGBA32(0xA0, 0xA0, 0xB0, 0xFF),
                      "t3dm v%d  %u verts  24 strip idx", model->magic[3],
                      model->totalVertCount);
      kiln_gui_end();
    kiln_frame_end();

    const KilnHostT3DCounters *t = kiln_host_t3d_counters();
    printf("  drew: loads %u verts %u submitted %u drawn %u culled %u clipped %u\n",
           t->vert_loads, t->verts, t->tris_submitted, t->tris_drawn,
           t->tris_culled, t->tris_clipped);

    /* Six quads, two triangles each. Getting 22 instead would mean the strip
     * restart flag was ignored and the whole cube was decoded as one run. */
    CHECK(t->tris_submitted == 12, "%u triangles submitted, expected 12 "
          "(6 quads x 2). A different number means the strip restart flag "
          "(bit 15) was misread.", t->tris_submitted);
    CHECK(t->vert_loads == 1, "%u vertex loads, expected 1 (24 verts fits the "
          "70-entry cache)", t->vert_loads);
    CHECK(t->tris_drawn >= 6, "only %u triangles produced pixels", t->tris_drawn);

    kiln_host_stats(stdout, 5);
    CHECK(kiln_host_capture(png) == 0, "could not write %s", png);

    t3d_model_free(model);

    if (fails) { printf("\nFAILED (%d)\n", fails); return 1; }
    printf("a real .t3dm parsed and rendered on the host\n");
    return 0;
}
