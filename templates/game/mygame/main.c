// SPDX-License-Identifier: MIT
//
// mygame — the starting point for a game on the Kiln engine.
//
// One frame of the engine's two passes: a lit 3D scene (a checker floor and a
// box you can turn with the stick), then a 2D HUD on top. Everything a Kiln game
// does happens between these same brackets:
//
//   kiln_frame_begin()
//     kiln_scene_begin(&scene)   3D: Tiny3D, perspective, lit, depth-tested
//     kiln_gui_begin()           2D: panels and text, depth off
//     kiln_gui_end()
//   kiln_frame_end()
//
// Next steps: kiln_actor for game objects, kiln_clip for collision, kiln_audio
// for sound, kiln_skel for animated characters. Kiln's CLAUDE.md maps them all.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_prim.h>

#define SCREEN_W 320
#define SCREEN_H 240

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    kiln_input_init();

    KilnScene scene;
    kiln_scene_init(&scene);
    kiln_prim_stage(&scene, RGBA32(0x40, 0x58, 0x80, 0xFF), 200.0f, 500.0f);
    scene.cam_pos = (fm_vec3_t){{ 0, 70, -150 }};
    scene.cam_target = (fm_vec3_t){{ 0, 18, 0 }};

    KilnPrim floor, box;
    kiln_prim_floor(&floor, 160.0f, 8, kiln_prim_rgba(0xD8, 0xD0, 0xB8), kiln_prim_rgba(0xA8, 0xA0, 0x88));
    kiln_prim_box(&box, (fm_vec3_t){{ 0, 18, 0 }}, (fm_vec3_t){{ 18, 18, 18 }},
                  kiln_prim_rgba(0xFF, 0xB0, 0x50), kiln_prim_rgba(0xD0, 0x70, 0x30), kiln_prim_rgba(0x60, 0x30, 0x18));

    KilnTransform box_xf;
    kiln_transform_init(&box_xf);
    box_xf.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};

    for (uint32_t frame = 0;; frame++) {
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);
        box_xf.rot_angle += 0.02f + in->stick_x * 0.08f;
        kiln_scene_update(&scene);

        kiln_frame_begin();
        kiln_scene_begin(&scene);
        kiln_prim_draw(&floor);
        kiln_transform_push(&box_xf);
        kiln_prim_draw(&box);
        kiln_transform_pop();

        kiln_gui_begin();
        kiln_gui_panel(8, 8, 150, 30, RGBA32(0x14, 0x18, 0x24, 0xFF), RGBA32(0xFF, 0xC8, 0x60, 0xFF));
        kiln_gui_text(14, 21, RGBA32(0xFF, 0xC8, 0x60, 0xFF), "MY GAME");
        kiln_gui_text(14, 33, RGBA32(0xE8, 0xE8, 0xF0, 0xFF), "frame %lu", (unsigned long)frame);
        kiln_gui_end();
        kiln_frame_end();
    }
}
