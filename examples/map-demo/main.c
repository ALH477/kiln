// SPDX-License-Identifier: MPL-2.0
//
// Phase C step 2: m64_dict + m64_map. Loads the Quake-format
// `assets/quake_test.map` (already in the ROM via the asset pipeline), parses
// it into M64Brush collision + M64MapFace render geometry, and spawns a player
// from the `info_player_start` entity by reading its "origin" vec3 out of the
// M64Dict. The player is a red box that reads the analog stick through
// m64_input and slides against the worldspawn brushes via m64_clip.
//
//   m64_map  -> parse .map: epairs -> M64Dict, brushes -> AABB + face quads
//   m64_dict -> typed spawn args: "origin" "0 0 0", "angle" "0"
//   m64_clip -> use the parsed brushes as the collision world

#include <libdragon.h>
#include <m64/m64_engine.h>
#include <m64/m64_gui.h>
#include <m64/m64_input.h>
#include <m64/m64_clip.h>
#include <m64/m64_dict.h>
#include <m64/m64_map.h>
#include <m64/m64_actor.h>

#include <malloc.h>

#define SCREEN_W 320
#define SCREEN_H 240

static const uint8_t CUBE_TRIS[12][3] = {
    {0,1,2},{2,3,0}, {4,6,5},{6,4,7},
    {0,4,5},{5,1,0}, {1,5,6},{6,2,1},
    {2,6,7},{7,3,2}, {3,7,4},{4,0,3},
};

static T3DVertPacked *make_unit_cube(uint32_t rgba)
{
    T3DVertPacked *v = malloc_uncached(sizeof(T3DVertPacked) * 4);
    const int16_t s = 1;
    const int16_t c[8][3] = {
        {-s,-s,-s},{ s,-s,-s},{ s, s,-s},{-s, s,-s},
        {-s,-s, s},{ s,-s, s},{ s, s, s},{-s, s, s},
    };
    for (int i = 0; i < 8; i += 2) {
        fm_vec3_t na = {{ (float)c[i][0],   (float)c[i][1],   (float)c[i][2]   }};
        fm_vec3_t nb = {{ (float)c[i+1][0], (float)c[i+1][1], (float)c[i+1][2] }};
        fm_vec3_norm(&na, &na);
        fm_vec3_norm(&nb, &nb);
        v[i / 2] = (T3DVertPacked){
            .posA = { c[i][0],   c[i][1],   c[i][2]   }, .rgbaA = rgba,
            .normA = t3d_vert_pack_normal(&na),
            .posB = { c[i+1][0], c[i+1][1], c[i+1][2] }, .rgbaB = rgba,
            .normB = t3d_vert_pack_normal(&nb),
        };
    }
    return v;
}

static void draw_box(const T3DVertPacked *verts, fm_vec3_t center, fm_vec3_t half)
{
    M64Transform t;
    m64_transform_init(&t);
    t.pos = center;
    t.scale = half;
    m64_transform_push(&t);
    t3d_vert_load(verts, 0, 8);
    for (int i = 0; i < 12; i++)
        t3d_tri_draw(CUBE_TRIS[i][0], CUBE_TRIS[i][1], CUBE_TRIS[i][2]);
    t3d_tri_sync();
    m64_transform_pop();
    m64_transform_free(&t);
}

int main(void)
{
    m64_engine_init(RESOLUTION_320x240);
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);
    m64_input_init();

    /* Register the one actor type the map can spawn. */
    m64_map_register_classname("info_player_start", 0);

    M64Map map;
    if (m64_map_load(&map, "rom:/maps/quake_test.map") < 0) {
        /* If the asset wasn't found, fall back to a hard-coded origin so the
         * demo still renders the room and the failure is visible in the HUD. */
        debugf("map-demo: m64_map_load failed\n");
    }

    m64_clip_set_world(map.brushes, map.brush_count);

    M64Scene scene;
    m64_scene_init(&scene);
    scene.far_z = 400.0f;

    /* Spawn origin from the map's info_player_start dict, or default. */
    fm_vec3_t player_pos = (map.spawn_count > 0)
        ? map.spawns[0].pos
        : (fm_vec3_t){{ 0, 8, 0 }};
    float player_yaw = (map.spawn_count > 0) ? map.spawns[0].yaw : 0.0f;

    /* Camera over the room; the parsed cube is 128 units across (-64..64). */
    scene.cam_pos    = (fm_vec3_t){{  100, 110, -100 }};
    scene.cam_target = (fm_vec3_t){{    0,   8,    0 }};
    m64_scene_update(&scene);

    T3DVertPacked *plyr_v = make_unit_cube(0xFF4C6AFF);
    const fm_vec3_t half = (fm_vec3_t){{ 8, 8, 8 }};
    const float speed = 60.0f;

    fm_vec3_t fwd  = {{ scene.cam_target.v[0] - scene.cam_pos.v[0], 0,
                        scene.cam_target.v[2] - scene.cam_pos.v[2] }};
    fm_vec3_norm(&fwd, &fwd);
    fm_vec3_t right = {{ -fwd.v[2], 0, fwd.v[0] }};

    uint32_t frames = 0;
    float fps = 0.0f;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        m64_input_update();
        const M64Input *in = m64_input_get(1);

        const float dt = 1.0f / 60.0f;
        fm_vec3_t vel = {{
            (fwd.v[0]   * in->stick_y + right.v[0] * in->stick_x) * speed,
            0.0f,
            (fwd.v[2]   * in->stick_y + right.v[2] * in->stick_x) * speed,
        }};
        fm_vec3_t disp = {{ vel.v[0] * dt, 0, vel.v[2] * dt }};
        player_pos = m64_clip_slide(player_pos, disp, half, half, 4);
        player_yaw = fm_atan2f(vel.v[0], vel.v[2]);

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        /* ── 3D pass ───────────────────────────────────────────────── */
        m64_frame_begin();
        m64_scene_begin(&scene);

        m64_map_draw(&map);
        draw_box(plyr_v, player_pos, half);

        /* ── 2D pass ───────────────────────────────────────────────── */
        m64_gui_begin();
        m64_gui_panel(8, 8, 210, 84,
                      RGBA32(10, 10, 24, 200), RGBA32(0, 245, 212, 255));
        m64_gui_text(14, 22, RGBA32(0, 245, 212, 255), "M64 MAP + DICT");
        m64_gui_text(14, 34, RGBA32(232, 232, 240, 255), "fps  %5.1f", fps);
        m64_gui_text(14, 46, RGBA32(232, 232, 240, 255),
                     "spawns %d  faces %d  brushes %d",
                     map.spawn_count, map.face_count, map.brush_count);
        if (map.spawn_count > 0) {
            m64_gui_text(14, 58, RGBA32(232, 232, 240, 255),
                         "origin %6.1f %6.1f %6.1f",
                         map.spawns[0].pos.v[0], map.spawns[0].pos.v[1], map.spawns[0].pos.v[2]);
            const char *ang = m64_dict_get_str(&map.spawns[0].dict, "angle", "?");
            m64_gui_text(14, 70, RGBA32(232, 232, 240, 255), "angle %s", ang);
        }
        m64_gui_panel(8, SCREEN_H - 28, SCREEN_W - 16, 20,
                      RGBA32(10, 10, 24, 200), RGBA32(139, 92, 246, 255));
        m64_gui_text(14, SCREEN_H - 18, RGBA32(232, 232, 240, 255),
                     "stick: move inside the .map room");
        m64_gui_end();
        m64_frame_end();
    }
}