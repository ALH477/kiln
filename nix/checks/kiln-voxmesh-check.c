/* SPDX-License-Identifier: MIT
 *
 * Renders a real voxel mesh — kiln_voxel's greedy quads through
 * kiln_voxmesh's packer through the host 3D pass — twice, with two different
 * colour combiners, and captures both.
 *
 * ── Why twice ─────────────────────────────────────────────────────────
 * Because the difference WAS a live defect in Forge, and this is what made it
 * visible instead of arguable. The defect is fixed; the pair stays.
 *
 * kiln_voxmesh puts the BLOCK TYPE only into the atlas: type N takes tile N-1
 * and the UVs point at it (kiln_voxmesh.c:103-106). Vertex colour carries
 * nothing but per-face brightness — DIR_SHADE[dir], greyscale (:108). So the
 * atlas is the only thing that distinguishes one block type from another.
 *
 * Forge sets T3D_FLAG_TEXTURED, which makes the RSP emit texture coordinates —
 * but the COMBINER is what decides whether the texel survives, and Tiny3D's
 * t3d_state_set_drawflags does not touch it (it only encodes the RSP triangle
 * command, t3d.c:300). For a long time nothing in Forge/src set one either, so
 * kiln_scene_begin's RDPQ_COMBINER_SHADE (kiln_engine.c:126) stood: output =
 * vertex colour, texel discarded. Every one of Forge's fifteen block types
 * drew the same grey, PAINT mode's atlas never reached the screen, and `Z`'s
 * veiled-palette preview could not change the geometry it was previewing.
 *
 * Forge's begin_voxel_state now sets RDPQ_COMBINER_TEX_SHADE, so `-texshade`
 * is what the editor draws and `-shade` is kept as the counter-example.
 *
 * ── What this body can and cannot see ─────────────────────────────────
 * It sets both combiners ITSELF and never compiles or reads Forge, so it pins
 * what the two combiners DO and is blind to which one Forge picks. Removing
 * the call from begin_voxel_state leaves every capture here matching while the
 * editor goes back to grey. kiln-voxmesh.nix greps forge_geo.c for exactly
 * that reason — the assertion the pixels cannot make.
 */
#include <kiln_voxel.h>
#include <kiln_voxmesh.h>
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

#define ARENA_VERTS 4096
static KilnVoxelWorld  g_world;
static KilnVoxelQuad   g_quads[2048];
static T3DVertPacked   g_arena_mem[ARENA_VERTS / 2];
static KilnVoxMeshArena g_arena;
static KilnVoxMesh     g_mesh;
static KilnVoxAtlas    g_atlas;

/* A stepped platform of five different block types, so "all types look the
 * same" is a thing you can see rather than infer. */
static void build_world(void)
{
    for (int x = 0; x < 10; x++)
        for (int z = 0; z < 10; z++)
            kiln_voxel_set(&g_world, x, 0, z, (uint8_t)(1 + ((x / 2) % 5)));
    for (int x = 2; x < 8; x++)
        for (int z = 2; z < 8; z++)
            kiln_voxel_set(&g_world, x, 1, z, (uint8_t)(1 + ((z / 2) % 5)));
    for (int x = 4; x < 6; x++)
        for (int z = 4; z < 6; z++)
            kiln_voxel_set(&g_world, x, 2, z, 5);
}

static void render(KilnScene *s, rdpq_combiner_t comb, const char *label,
                   const char *png)
{
    kiln_frame_begin();
      kiln_scene_begin(s);
        /* Exactly Forge's GEO setup, then the combiner under test. */
        t3d_state_set_drawflags(T3D_FLAG_TEXTURED | T3D_FLAG_SHADED |
                                T3D_FLAG_DEPTH);
        kiln_voxatlas_bind(&g_atlas, KILN_VOXATLAS_COLD);
        rdpq_mode_combiner(comb);
        kiln_voxmesh_draw(&g_mesh);
      kiln_gui_begin();
        kiln_gui_rect(0, 0, 320, 12, RGBA32(0x10, 0x12, 0x18, 0xFF));
        kiln_gui_text(6, 9, RGBA32(0x00, 0xF5, 0xD4, 0xFF), "%s", label);
        kiln_gui_text(6, 232, RGBA32(0xA0, 0xA0, 0xB0, 0xFF),
                      "5 block types  tmem %d/%d", kiln_host_tmem_used(),
                      TMEM_BYTES);
      kiln_gui_end();
    kiln_frame_end();

    const KilnHostT3DCounters *t = kiln_host_t3d_counters();
    printf("  %-28s tris %u drawn %u  texels-discarded %u\n",
           label, t->tris_submitted, t->tris_drawn, t->texels_discarded);
    CHECK(kiln_host_capture(png) == 0, "could not write %s", png);
}

int main(int argc, char **argv)
{
    const char *shade_png = argc > 1 ? argv[1] : "vox-shade.png";
    const char *tex_png   = argc > 2 ? argv[2] : "vox-texshade.png";

    kiln_engine_init(RESOLUTION_320x240);
    kiln_gui_init();

    memset(&g_world, 0, sizeof g_world);
    build_world();
    build_world();   /* idempotent: setting the same block twice must not grow anything */

    CHECK(kiln_voxatlas_init(&g_atlas) == 0, "atlas init failed");
    /* The atlas is 64x64 CI4 = 2 KB against a 4 KB TMEM, which is the whole
     * reason the format was chosen. Assert it, because a future 8-bit atlas
     * would fit and then not fit alongside anything else. */
    kiln_voxatlas_bind(&g_atlas, KILN_VOXATLAS_COLD);
    CHECK(kiln_host_tmem_used() == 2048,
          "atlas occupies %d TMEM bytes, expected 2048 (64x64 CI4)",
          kiln_host_tmem_used());

    const int nq = kiln_voxel_quads(&g_world, 0, g_quads,
                                    sizeof g_quads / sizeof *g_quads);
    CHECK(nq > 0, "kiln_voxel_quads returned %d", nq);
    printf("  greedy surface extraction: %d quads\n", nq);

    kiln_voxmesh_arena_init(&g_arena, g_arena_mem, ARENA_VERTS);
    CHECK(kiln_voxmesh_build(&g_world, 0, g_quads, (uint32_t)nq, &g_arena,
                             KILN_VOXATLAS_ACROSS, &g_mesh) == 0,
          "kiln_voxmesh_build failed");
    printf("  packed %u quads, arena %u%%\n", g_mesh.quad_count,
           kiln_voxmesh_arena_used_pct(&g_arena));

    KilnScene scene;
    kiln_scene_init(&scene);
    scene.cam_pos    = (fm_vec3_t){{ 330.0f, 300.0f, 380.0f }};
    scene.cam_target = (fm_vec3_t){{ 160.0f,  16.0f, 160.0f }};
    scene.fov_deg    = 50.0f;
    scene.near_z     = 10.0f;
    scene.far_z      = 2000.0f;
    scene.clear_color = RGBA32(0x08, 0x0A, 0x10, 0xFF);
    scene.ambient[0] = scene.ambient[1] = scene.ambient[2] = 0xFF;
    scene.ambient[3] = 0xFF;
    scene.light_count = 0;   /* the atlas and DIR_SHADE only, so nothing else
                              * can be mistaken for block-type colour */
    kiln_scene_update(&scene);

    render(&scene, RDPQ_COMBINER_SHADE, "COMBINER SHADE - as Forge draws",
           shade_png);
    const uint32_t discarded = kiln_host_t3d_counters()->texels_discarded;
    render(&scene, RDPQ_COMBINER_TEX_SHADE, "COMBINER TEX SHADE - with atlas",
           tex_png);
    const uint32_t used = kiln_host_t3d_counters()->texels_discarded;

    /* The finding, as an assertion. If Forge ever sets a TEX combiner this
     * fails, and the failure is the good news. */
    CHECK(discarded > 1000,
          "COMBINER_SHADE discarded only %u texels; the atlas may now be "
          "reaching the screen", discarded);
    CHECK(used == 0, "COMBINER_TEX_SHADE discarded %u texels; it should use "
          "all of them", used);

    if (fails) { printf("\nFAILED (%d)\n", fails); return 1; }
    printf("voxel mesh rendered both ways; the atlas is discarded under "
           "COMBINER_SHADE\n");
    return 0;
}
