/* SPDX-License-Identifier: MIT
 *
 * map_render.c — see map_render.h. Extracted verbatim from
 * nix/checks/kiln-map-check.c; the check keeps only its assertions.
 */
#include "map_render.h"

#include <kiln_clip.h>
#include <kiln_engine.h>
#include <kiln_gui.h>
#include <kiln_debugdraw.h>
#include <kiln_host.h>
#include <libdragon.h>

#include <stdio.h>

void map_render_init(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    kiln_gui_init();
    dfs_init(DFS_DEFAULT_LOCATION);
}

int map_render_open(KilnMap *out, const char *dfs_path)
{
    const int rc = kiln_map_load(out, dfs_path);
    if (rc != 0) return rc;

    printf("  loaded '%s': %u brushes, %u faces, %u spawns\n",
           dfs_path, out->brush_count, out->face_count, out->spawn_count);
    printf("  aabb [%.0f %.0f %.0f] .. [%.0f %.0f %.0f]\n",
           (double)out->world_aabb_min.v[0], (double)out->world_aabb_min.v[1],
           (double)out->world_aabb_min.v[2], (double)out->world_aabb_max.v[0],
           (double)out->world_aabb_max.v[1], (double)out->world_aabb_max.v[2]);
    return 0;
}

int map_render_frame(const KilnMap *m, const char *png)
{
    /* Into the clip world, exactly as examples/oot-demo does at boot: a map is
     * geometry AND collision, and loading one without installing it is the
     * shape of the bug that made the player fall forever. */
    kiln_clip_set_world(m->brushes, m->brush_count);

    const fm_vec3_t centre = {{
        (m->world_aabb_min.v[0] + m->world_aabb_max.v[0]) * 0.5f,
        (m->world_aabb_min.v[1] + m->world_aabb_max.v[1]) * 0.5f,
        (m->world_aabb_min.v[2] + m->world_aabb_max.v[2]) * 0.5f }};
    const float span = m->world_aabb_max.v[0] - m->world_aabb_min.v[0]
                     + m->world_aabb_max.v[2] - m->world_aabb_min.v[2];

    KilnScene scene;
    kiln_scene_init(&scene);
    scene.cam_pos = (fm_vec3_t){{ centre.v[0] + span * 0.55f,
                                  m->world_aabb_max.v[1] + span * 0.42f,
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
    scene.light_dir = (fm_vec3_t){{ 0.45f, 0.8f, 0.4f }};
    scene.light_count = 1;
    kiln_scene_update(&scene);

    kiln_frame_begin();
      kiln_scene_begin(&scene);
        kiln_map_draw(m);
      kiln_gui_begin();
        /* The brush AABBs through kiln_debugdraw, which projects with
         * kiln_scene_project — the pure-fm_ path that needed no 3D backend at
         * all. Drawn in the 2D pass so it cannot perturb the frame it
         * describes. */
        kiln_dd_begin(&scene, 320, 240);
        for (uint16_t i = 0; i < m->brush_count && i < 24; i++)
            kiln_dd_aabb(m->brushes[i].mins, m->brushes[i].maxs,
                         RGBA32(0x00, 0xF5, 0xD4, 0xFF));
        kiln_dd_end();
        kiln_gui_rect(0, 0, 320, 12, RGBA32(0x10, 0x12, 0x18, 0xFF));
        kiln_gui_text(6, 9, RGBA32(0x00, 0xF5, 0xD4, 0xFF), "KILN MAP");
        kiln_gui_text(6, 232, RGBA32(0xA0, 0xA0, 0xB0, 0xFF),
                      "brushes %u  faces %u  clip %d", m->brush_count,
                      m->face_count, kiln_clip_world_count());
      kiln_gui_end();
    kiln_frame_end();

    const KilnHostT3DCounters *t = kiln_host_t3d_counters();
    printf("  drew: loads %u submitted %u drawn %u clipped %u\n",
           t->vert_loads, t->tris_submitted, t->tris_drawn, t->tris_clipped);

    kiln_host_stats(stdout, 5);
    return kiln_host_capture(png);
}
