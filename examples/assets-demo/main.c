// SPDX-License-Identifier: MIT
//
// Phase A verification: a ROM that loads one of each asset kind the pipeline
// (nix/assets.nix) converts, rather than the hand-built geometry every other
// example uses. If gltf_to_t3d, mksprite or audioconv64 silently produced a
// file the runtime can't actually load, this is where that would show up.
//
// Model and sprite go through StreamDB (kiln_asset), demonstrating the
// realistic mixed pattern most content actually wants — see CLAUDE.md's
// "Datafiles: StreamDB vs loose DFS". blip.wav64 stays loose DFS because
// kiln_asset_wav64 doesn't exist: wav64_open has no in-memory variant.
//
//   models/cube.t3dm   -> kiln_asset_model, drawn in the 3D pass
//   sprites/logo.sprite -> kiln_asset_sprite, GUI pass via rdpq_sprite_upload
//   sfx/blip.wav64      -> wav64, one-shot on A (loose DFS, see above)

#include <libdragon.h>
#include <t3d/t3dmodel.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_asset.h>

#include <malloc.h>
#include <string.h>

#define SCREEN_W 320
#define SCREEN_H 240
#define SAMPLE_RATE 32000
#define CH_BLIP 0

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();

    dfs_init(DFS_DEFAULT_LOCATION);

    audio_init(SAMPLE_RATE, 4);
    mixer_init(1);
    wav64_t blip;
    wav64_open(&blip, "rom:/sfx/blip.wav64");

    size_t need = kiln_asset_probe_size("rom:/assets-demo.streamdb");
    assertf(need > 0, "assets-demo: assets-demo.streamdb not found / probe failed");
    void *arena = malloc(need);
    assertf(arena, "assets-demo: arena malloc %zu failed", need);
    KilnAsset *db = kiln_asset_open("rom:/assets-demo.streamdb", arena, need);
    assertf(db, "assets-demo: kiln_asset_open failed");

    T3DModel *model = kiln_asset_model(db, "models/cube.t3dm", strlen("models/cube.t3dm"));
    assertf(model, "assets-demo: kiln_asset_model failed");

    sprite_t *logo = kiln_asset_sprite(db, "sprites/logo.sprite", strlen("sprites/logo.sprite"));
    assertf(logo, "assets-demo: kiln_asset_sprite failed");

    KilnScene scene;
    kiln_scene_init(&scene);
    scene.cam_pos = (fm_vec3_t){{ 0, 14, -70 }};
    scene.far_z = 300.0f;

    KilnTransform xform;
    kiln_transform_init(&xform);
    xform.scale = (fm_vec3_t){{ 1.0f, 1.0f, 1.0f }};

    float spin = 0.0f;

    for (;;) {
        joypad_poll();
        joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);
        if (pressed.a) {
            wav64_play(&blip, CH_BLIP);
        }

        spin += 0.02f;
        kiln_scene_update(&scene);

        xform.rot_angle = spin;
        xform.rot_axis = (fm_vec3_t){{ 0.3f, 1.0f, 0.15f }};
        fm_vec3_norm(&xform.rot_axis, &xform.rot_axis);

        kiln_frame_begin();
        kiln_scene_begin(&scene);

        kiln_transform_push(&xform);
        t3d_model_draw(model);
        kiln_transform_pop();

        kiln_gui_begin();

        rdpq_sprite_upload(TILE0, logo, NULL);
        rdpq_texture_rectangle(TILE0, SCREEN_W - 40, 8, SCREEN_W - 8, 40, 0, 0);

        kiln_gui_panel(8, 8, 150, 34,
                      RGBA32(10, 10, 24, 200), RGBA32(0, 245, 212, 255));
        kiln_gui_text(14, 22, RGBA32(0, 245, 212, 255), "KILN ASSETS");
        kiln_gui_text(14, 34, RGBA32(232, 232, 240, 255), "A: play blip.wav64");

        kiln_gui_end();
        kiln_frame_end();

        while (audio_can_write()) {
            short *buf = audio_write_begin();
            mixer_poll(buf, audio_get_buffer_length());
            audio_write_end();
        }
    }
}
