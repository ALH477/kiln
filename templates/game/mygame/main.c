// SPDX-License-Identifier: MIT
//
// mygame — the starting point for a game on Figulina.
//
// One frame of the engine's two passes: a lit 3D scene (a checker floor and a
// box you can turn with the stick), then a 2D HUD on top. Everything a Kiln game
// does happens between these same brackets:
//
//   fig_frame_begin()
//     fig_scene_begin(&scene)   3D: Tiny3D, perspective, lit, depth-tested
//     fig_gui_begin()           2D: panels and text, depth off
//     fig_gui_end()
//   fig_frame_end()
//
// Next steps: fig_actor for game objects, fig_clip for collision, fig_audio
// for sound, fig_skel for animated characters. Kiln's CLAUDE.md maps them all.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_prim.h>

#define SCREEN_W 320
#define SCREEN_H 240

int main(void)
{
    fig_engine_init(RESOLUTION_320x240);
    joypad_init();
    fig_input_init();

    FigScene scene;
    fig_scene_init(&scene);
    fig_prim_stage(&scene, RGBA32(0x40, 0x58, 0x80, 0xFF), 200.0f, 500.0f);
    scene.cam_pos = (fm_vec3_t){{ 0, 70, -150 }};
    scene.cam_target = (fm_vec3_t){{ 0, 18, 0 }};

    FigPrim floor, box;
    fig_prim_floor(&floor, 160.0f, 8, fig_prim_rgba(0xD8, 0xD0, 0xB8), fig_prim_rgba(0xA8, 0xA0, 0x88));
    fig_prim_box(&box, (fm_vec3_t){{ 0, 18, 0 }}, (fm_vec3_t){{ 18, 18, 18 }},
                  fig_prim_rgba(0xFF, 0xB0, 0x50), fig_prim_rgba(0xD0, 0x70, 0x30), fig_prim_rgba(0x60, 0x30, 0x18));

    FigTransform box_xf;
    fig_transform_init(&box_xf);
    box_xf.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};

    for (uint32_t frame = 0;; frame++) {
        fig_input_update();
        const FigInput *in = fig_input_get(1);
        box_xf.rot_angle += 0.02f + in->stick_x * 0.08f;
        fig_scene_update(&scene);

        fig_frame_begin();
        fig_scene_begin(&scene);
        fig_prim_draw(&floor);
        fig_transform_push(&box_xf);
        fig_prim_draw(&box);
        fig_transform_pop();

        fig_gui_begin();
        fig_gui_panel(8, 8, 150, 30, RGBA32(0x14, 0x18, 0x24, 0xFF), RGBA32(0xFF, 0xC8, 0x60, 0xFF));
        fig_gui_text(14, 21, RGBA32(0xFF, 0xC8, 0x60, 0xFF), "MY GAME");
        fig_gui_text(14, 33, RGBA32(0xE8, 0xE8, 0xF0, 0xFF), "frame %lu", (unsigned long)frame);
        fig_gui_end();
        fig_frame_end();
    }
}
