// SPDX-License-Identifier: MPL-2.0
//
// Phase A verification: a ROM that loads one of each asset kind the pipeline
// (nix/assets.nix) converts, rather than the hand-built geometry every other
// example uses. If gltf_to_t3d, mksprite or audioconv64 silently produced a
// file the runtime can't actually load, this is where that would show up.
//
//   models/cube.t3dm   -> t3d_model_load, drawn in the 3D pass
//   sprites/logo.sprite -> rdpq_sprite_upload + rdpq_texture_rectangle, GUI pass
//   sfx/blip.wav64      -> wav64, one-shot on A

#include <libdragon.h>
#include <t3d/t3dmodel.h>
#include <m64/m64_engine.h>
#include <m64/m64_gui.h>

#define SCREEN_W 320
#define SCREEN_H 240
#define SAMPLE_RATE 32000
#define CH_BLIP 0

int main(void)
{
    m64_engine_init(RESOLUTION_320x240);
    joypad_init();

    dfs_init(DFS_DEFAULT_LOCATION);

    // mkModel runs `mkasset -c 2` over the .t3dm, matching what every Tiny3D
    // example does. Level 2 is not linked in by default — without this call
    // asset_load() cannot decompress it and t3d_model_load returns garbage.
    asset_init_compression(2);

    audio_init(SAMPLE_RATE, 4);
    mixer_init(1);
    wav64_t blip;
    wav64_open(&blip, "rom:/sfx/blip.wav64");

    T3DModel *model = t3d_model_load("rom:/models/cube.t3dm");

    sprite_t *logo = sprite_load("rom:/sprites/logo.sprite");

    M64Scene scene;
    m64_scene_init(&scene);
    scene.cam_pos = (fm_vec3_t){{ 0, 14, -70 }};
    scene.far_z = 300.0f;

    M64Transform xform;
    m64_transform_init(&xform);
    xform.scale = (fm_vec3_t){{ 1.0f, 1.0f, 1.0f }};

    float spin = 0.0f;

    for (;;) {
        joypad_poll();
        joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);
        if (pressed.a) {
            wav64_play(&blip, CH_BLIP);
        }

        spin += 0.02f;
        m64_scene_update(&scene);

        xform.rot_angle = spin;
        xform.rot_axis = (fm_vec3_t){{ 0.3f, 1.0f, 0.15f }};
        fm_vec3_norm(&xform.rot_axis, &xform.rot_axis);

        m64_frame_begin();
        m64_scene_begin(&scene);

        m64_transform_push(&xform);
        t3d_model_draw(model);
        m64_transform_pop();

        m64_gui_begin();

        rdpq_sprite_upload(TILE0, logo, NULL);
        rdpq_texture_rectangle(TILE0, SCREEN_W - 40, 8, SCREEN_W - 8, 40, 0, 0);

        m64_gui_panel(8, 8, 150, 34,
                      RGBA32(10, 10, 24, 200), RGBA32(0, 245, 212, 255));
        m64_gui_text(14, 22, RGBA32(0, 245, 212, 255), "M64 ASSETS");
        m64_gui_text(14, 34, RGBA32(232, 232, 240, 255), "A: play blip.wav64");

        m64_gui_end();
        m64_frame_end();

        while (audio_can_write()) {
            short *buf = audio_write_begin();
            mixer_poll(buf, audio_get_buffer_length());
            audio_write_end();
        }
    }
}
