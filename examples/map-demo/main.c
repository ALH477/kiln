// SPDX-License-Identifier: MIT
//
// Phase C step 2: kiln_dict + kiln_map. Loads the Quake-format
// `assets/quake_test.map` (already in the ROM via the asset pipeline), parses
// it into KilnBrush collision + KilnMapFace render geometry, and spawns a player
// from the `info_player_start` entity by reading its "origin" vec3 out of the
// KilnDict. The player is a red box that reads the analog stick through
// kiln_input and slides against the worldspawn brushes via kiln_clip.
//
//   kiln_map  -> parse .map: epairs -> KilnDict, brushes -> AABB + face quads
//   kiln_dict -> typed spawn args: "origin" "0 0 0", "angle" "0"
//   kiln_clip -> use the parsed brushes as the collision world

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_clip.h>
#include <kiln/kiln_dict.h>
#include <kiln/kiln_map.h>
#include <kiln/kiln_actor.h>

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
    KilnTransform t;
    kiln_transform_init(&t);
    t.pos = center;
    t.scale = half;
    kiln_transform_push(&t);
    t3d_vert_load(verts, 0, 8);
    for (int i = 0; i < 12; i++)
        t3d_tri_draw(CUBE_TRIS[i][0], CUBE_TRIS[i][1], CUBE_TRIS[i][2]);
    t3d_tri_sync();
    kiln_transform_pop();
    kiln_transform_free(&t);
}

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);
    kiln_input_init();

    /* Register the one actor type the map can spawn. */
    kiln_map_register_classname("info_player_start", 0);

    /* Zero-initialised, because the failure path below READS this struct. It was
     * declared uninitialised, and on failure `map.brush_count` and `map.brushes`
     * were whatever was on the stack — which then went straight into
     * kiln_clip_set_world as a count and a pointer. The comment already promised
     * the demo would "still render the room and make the failure visible in the
     * HUD"; it could not, because there was nothing valid to read. */
    KilnMap map = { 0 };
    if (kiln_map_load(&map, "rom:/maps/quake_test.map") < 0) {
        /* If the asset wasn't found, fall back to a hard-coded origin so the
         * demo still renders the room and the failure is visible in the HUD. */
        debugf("map-demo: kiln_map_load failed\n");
    }

    kiln_clip_set_world(map.brushes, map.brush_count);

    KilnScene scene;
    kiln_scene_init(&scene);
    scene.far_z = 400.0f;

    /* Spawn origin from the map's info_player_start dict, or default. */
    fm_vec3_t player_pos = (map.spawn_count > 0)
        ? map.spawns[0].pos
        : (fm_vec3_t){{ 0, 8, 0 }};
    float player_yaw = (map.spawn_count > 0) ? map.spawns[0].yaw : 0.0f;

    /* Camera over the room; the parsed cube is 128 units across (-64..64). */
    scene.cam_pos    = (fm_vec3_t){{  100, 110, -100 }};
    scene.cam_target = (fm_vec3_t){{    0,   8,    0 }};
    kiln_scene_update(&scene);

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
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);

        const float dt = 1.0f / 60.0f;
        fm_vec3_t vel = {{
            (fwd.v[0]   * in->stick_y + right.v[0] * in->stick_x) * speed,
            0.0f,
            (fwd.v[2]   * in->stick_y + right.v[2] * in->stick_x) * speed,
        }};
        fm_vec3_t disp = {{ vel.v[0] * dt, 0, vel.v[2] * dt }};
        player_pos = kiln_clip_slide(player_pos, disp, half, half, 4);
        /* Only when actually moving. fm_atan2f(0, 0) is an invalid operation on
         * the VR4300 and halts the ROM in libdragon's fast-math atan2 — which is
         * what happened here on the very first frame, with the stick at rest, the
         * moment this demo was given the map it had never actually been shipping
         * (see the Makefile's "THE FILESYSTEM" comment). A resting player has no
         * facing to recompute, so keeping the last one is also the correct
         * behaviour rather than merely the safe one. */
        if (vel.v[0] != 0.0f || vel.v[2] != 0.0f)
            player_yaw = fm_atan2f(vel.v[0], vel.v[2]);

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        /* ── 3D pass ───────────────────────────────────────────────── */
        kiln_frame_begin();
        kiln_scene_begin(&scene);

        kiln_map_draw(&map);
        draw_box(plyr_v, player_pos, half);

        /* ── 2D pass ───────────────────────────────────────────────── */
        kiln_gui_begin();
        kiln_gui_panel(8, 8, 210, 84,
                      RGBA32(10, 10, 24, 200), RGBA32(0, 245, 212, 255));
        kiln_gui_text(14, 22, RGBA32(0, 245, 212, 255), "KILN MAP + DICT");
        kiln_gui_text(14, 34, RGBA32(232, 232, 240, 255), "fps  %5.1f", fps);
        kiln_gui_text(14, 46, RGBA32(232, 232, 240, 255),
                     "spawns %d  faces %d  brushes %d",
                     map.spawn_count, map.face_count, map.brush_count);
        if (map.spawn_count > 0) {
            kiln_gui_text(14, 58, RGBA32(232, 232, 240, 255),
                         "origin %6.1f %6.1f %6.1f",
                         map.spawns[0].pos.v[0], map.spawns[0].pos.v[1], map.spawns[0].pos.v[2]);
            const char *ang = kiln_dict_get_str(&map.spawns[0].dict, "angle", "?");
            kiln_gui_text(14, 70, RGBA32(232, 232, 240, 255), "angle %s", ang);
        }
        kiln_gui_panel(8, SCREEN_H - 28, SCREEN_W - 16, 20,
                      RGBA32(10, 10, 24, 200), RGBA32(139, 92, 246, 255));
        kiln_gui_text(14, SCREEN_H - 18, RGBA32(232, 232, 240, 255),
                     "stick: move inside the .map room");
        kiln_gui_end();
        kiln_frame_end();
    }
}