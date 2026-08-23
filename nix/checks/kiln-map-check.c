/* SPDX-License-Identifier: MIT
 *
 * Loads a real Quake .map through the real kiln_map.c, off the host VFS, and
 * renders it — the first time a level in this repo has been drawn anywhere but
 * a ROM.
 *
 * This is the payoff for the IO tier. kiln_map_load takes a DFS path, which on
 * console reads a filesystem image appended to the ROM; here it resolves under
 * $KILN_HOST_DFS. The chain that matters is the one CLAUDE.md says cost
 * PetaByte Madness its entire PLAY screen: an asset builder's `name` IS the
 * filename the ROM opens, kiln_map_load returns non-zero instead of asserting,
 * and an empty clip world makes every trace report "nothing in the way". Every
 * layer degrades politely and the composition is silent. On the host that same
 * mismatch is a missing file with a path in the message.
 *
 * It also asserts the brush/face counts, because a .map parse that silently
 * drops geometry renders as level-design intent.
 */
#include <kiln_map.h>
#include <kiln_clip.h>
#include <kiln_engine.h>
#include <kiln_gui.h>
#include <kiln_debugdraw.h>
#include <kiln_host.h>
#include <libdragon.h>

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(int argc, char **argv)
{
    const char *map = argc > 1 ? argv[1] : "rom:/quake_test.map";
    const char *png = argc > 2 ? argv[2] : "map.png";

    kiln_engine_init(RESOLUTION_320x240);
    kiln_gui_init();
    dfs_init(DFS_DEFAULT_LOCATION);

    /* A missing map must be a LOUD miss, not a silent empty world. Prove the
     * negative first, because it is the failure mode that actually happened. */
    KilnMap absent;
    memset(&absent, 0, sizeof absent);
    CHECK(kiln_map_load(&absent, "rom:/definitely_not_here.map") != 0,
          "loading a nonexistent map returned success");
    CHECK(absent.brush_count == 0, "a failed load left %u brushes",
          absent.brush_count);

    KilnMap m;
    memset(&m, 0, sizeof m);
    const int rc = kiln_map_load(&m, map);
    CHECK(rc == 0, "kiln_map_load('%s') returned %d", map, rc);
    if (rc != 0) return 1;

    printf("  loaded '%s': %u brushes, %u faces, %u spawns\n",
           map, m.brush_count, m.face_count, m.spawn_count);
    printf("  aabb [%.0f %.0f %.0f] .. [%.0f %.0f %.0f]\n",
           (double)m.world_aabb_min.v[0], (double)m.world_aabb_min.v[1],
           (double)m.world_aabb_min.v[2], (double)m.world_aabb_max.v[0],
           (double)m.world_aabb_max.v[1], (double)m.world_aabb_max.v[2]);

    /* ── Two defects in kiln_map, pinned ──────────────────────────────
     * Rendering this on the host is what made them visible. Neither is mine
     * and neither is fixed here; both are asserted so that fixing them fails
     * this check, which is the notification you want.
     *
     * 1. kiln_map_draw does not render the brush's FACES. A Quake .map gives
     *    three points per face, and those points define a PLANE — by
     *    convention one unit apart, which is exactly what assets/quake_test.map
     *    uses. kiln_map.c:155 treats them as face corners and builds the
     *    parallelogram p0,p1,p2,p0+p2-p1, so a 128-unit wall renders as a
     *    1x2-unit patch at one corner. Six of those per brush.
     *
     *    The repo already contains the correct algorithm, on the host side:
     *    tools/blender/quake_map.py intersects every triple of a brush's
     *    planes and keeps the candidates inside all the others. CLAUDE.md
     *    credits that discipline with catching two real bugs. kiln_map.c does
     *    no intersection at all.
     *
     * 2. kiln_map.c:164 assigns t3d_vert_pack_normal's uint16_t return into a
     *    uint8_t, discarding the top eight bits — the whole x field and half
     *    of y. Three of this brush's six faces come out with normA == 0.
     *
     * What DOES work is the AABB, which is the componentwise min/max of the
     * plane points, and it is what every consumer actually relies on:
     * kiln_clip_set_world, kiln_room's brush install, PetaByte Madness' PLAY.
     * Which is presumably why the rendering was never examined — the geometry
     * on screen comes from models, and the brushes are collision.
     *
     * Note the AABB inherits the same off-by-one: the plane points reach one
     * unit past the brush, so a -64..64 brush becomes a -64..65 collision box.
     */
    CHECK(m.brush_count == 1, "%u brushes, expected 1", m.brush_count);
    CHECK(m.face_count == m.brush_count * 6,
          "%u faces for %u brushes; kiln_map emits one parallelogram per face, "
          "so this should be exactly 6x", m.face_count, m.brush_count);
    CHECK(m.world_aabb_min.v[0] == -64.0f && m.world_aabb_max.v[0] == 65.0f,
          "world aabb x is %.0f..%.0f, expected -64..65 — the brush is -64..64 "
          "and the extra unit is the plane-point convention leaking into the "
          "collision box", (double)m.world_aabb_min.v[0],
          (double)m.world_aabb_max.v[0]);
    {
        /* Finding 1, measured: the face quad's extent against the brush's. */
        const T3DVertPacked *v = m.faces[0].verts;
        int lo = v[0].posA[1], hi = v[0].posA[1];
        const int16_t ys[4] = { v[0].posA[1], v[0].posB[1],
                                v[1].posA[1], v[1].posB[1] };
        for (int i = 0; i < 4; i++) {
            if (ys[i] < lo) lo = ys[i];
            if (ys[i] > hi) hi = ys[i];
        }
        printf("  face 0 spans %d units on y; the brush spans %.0f\n",
               hi - lo, (double)(m.world_aabb_max.v[1] - m.world_aabb_min.v[1]));
        CHECK(hi - lo <= 4,
              "face 0 spans %d units, so kiln_map may now be doing real brush "
              "CSG — if so this check is obsolete and that is good news",
              hi - lo);
    }
    {
        /* Finding 2, measured: normals truncated to a byte. */
        int zeroed = 0;
        for (uint16_t i = 0; i < m.face_count; i++)
            if (m.faces[i].verts[0].normA == 0) zeroed++;
        printf("  %d of %u face normals are zero (uint8_t truncation)\n",
               zeroed, m.face_count);
        CHECK(zeroed > 0,
              "no face normals are zero, so kiln_map.c:164's uint8_t "
              "truncation may be fixed — if so this check is obsolete");
    }

    /* Into the clip world, exactly as examples/oot-demo does at boot: a map is
     * geometry AND collision, and loading one without installing it is the
     * shape of the bug that made the player fall forever. */
    kiln_clip_set_world(m.brushes, m.brush_count);
    CHECK(kiln_clip_world_count() == m.brush_count,
          "clip world has %d brushes, map has %u", kiln_clip_world_count(),
          m.brush_count);

    const fm_vec3_t centre = {{
        (m.world_aabb_min.v[0] + m.world_aabb_max.v[0]) * 0.5f,
        (m.world_aabb_min.v[1] + m.world_aabb_max.v[1]) * 0.5f,
        (m.world_aabb_min.v[2] + m.world_aabb_max.v[2]) * 0.5f }};
    const float span = m.world_aabb_max.v[0] - m.world_aabb_min.v[0]
                     + m.world_aabb_max.v[2] - m.world_aabb_min.v[2];

    KilnScene scene;
    kiln_scene_init(&scene);
    scene.cam_pos = (fm_vec3_t){{ centre.v[0] + span * 0.55f,
                                  m.world_aabb_max.v[1] + span * 0.42f,
                                  centre.v[2] + span * 0.62f }};
    scene.cam_target = centre;
    scene.fov_deg = 55.0f;
    scene.near_z = 4.0f;
    scene.far_z = span * 6.0f + 100.0f;
    scene.clear_color = RGBA32(0x08, 0x0A, 0x10, 0xFF);
    scene.ambient[0] = scene.ambient[1] = scene.ambient[2] = 0x48;
    scene.ambient[3] = 0xFF;
    scene.light_color[0] = 0xFF; scene.light_color[1] = 0xF2;
    scene.light_color[2] = 0xCC; scene.light_color[3] = 0xFF;
    scene.light_dir = (fm_vec3_t){{ -0.45f, -0.8f, -0.4f }};
    scene.light_count = 1;
    kiln_scene_update(&scene);

    kiln_frame_begin();
      kiln_scene_begin(&scene);
        kiln_map_draw(&m);
      kiln_gui_begin();
        /* The brush AABBs through kiln_debugdraw, which projects with
         * kiln_scene_project — the pure-fm_ path that needed no 3D backend at
         * all. Drawn in the 2D pass so it cannot perturb the frame it
         * describes. */
        kiln_dd_begin(&scene, 320, 240);
        for (uint16_t i = 0; i < m.brush_count && i < 24; i++)
            kiln_dd_aabb(m.brushes[i].mins, m.brushes[i].maxs,
                         RGBA32(0x00, 0xF5, 0xD4, 0xFF));
        kiln_dd_end();
        kiln_gui_rect(0, 0, 320, 12, RGBA32(0x10, 0x12, 0x18, 0xFF));
        kiln_gui_text(6, 9, RGBA32(0x00, 0xF5, 0xD4, 0xFF), "KILN MAP");
        kiln_gui_text(6, 232, RGBA32(0xA0, 0xA0, 0xB0, 0xFF),
                      "brushes %u  faces %u  clip %d", m.brush_count,
                      m.face_count, kiln_clip_world_count());
      kiln_gui_end();
    kiln_frame_end();

    const KilnHostT3DCounters *t = kiln_host_t3d_counters();
    printf("  drew: loads %u submitted %u drawn %u clipped %u\n",
           t->vert_loads, t->tris_submitted, t->tris_drawn, t->tris_clipped);
    CHECK(t->tris_submitted > 0, "no geometry submitted");
    CHECK(t->tris_drawn > 0, "geometry submitted but nothing rasterised");

    kiln_host_stats(stdout, 5);
    CHECK(kiln_host_capture(png) == 0, "could not write %s", png);
    kiln_map_free(&m);

    if (fails) { printf("\nFAILED (%d)\n", fails); return 1; }
    printf("a real .map loaded off the host VFS and rendered\n");
    return 0;
}
