// SPDX-License-Identifier: MIT
//
// Boots straight into the Kiln engine's boot splash: a classical brick
// kiln with a flame flickering at its doorway, assembling and settling on
// the beat, giving way to a publisher line lit by that same flame. See
// kiln_splash.h for the whole design and timing; this is exactly the
// four-call usage its own doc comment shows, plus a joypad so the splash's
// skip-on-any-button path is real rather than untested.
//
// No jingle: this example ships no baked instrument, and kiln_splash's own
// contract is that jingle_sfx may be -1 for silence with no change to the
// timing. Once the splash finishes, the screen holds on a plain label
// rather than going black — a capture taken any time after the sequence
// still shows something deliberate.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_splash.h>

#define SCREEN_W 320
#define SCREEN_H 240

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    kiln_gui_init();
    joypad_init();
    kiln_input_init();

    T3DModel *model = t3d_model_load("rom:/models/kiln_logo.t3dm");
    kiln_splash_init(model, -1, "Kiln Engine - MIT Licensed");

    for (;;) {
        joypad_poll();
        kiln_input_update();
        const KilnInput *in = kiln_input_get(0);

        KilnScene scene;
        kiln_scene_init(&scene);

        if (!kiln_splash_done()) {
            kiln_splash_update(1.0f / 60.0f, in);
            kiln_splash_apply(&scene);
        } else {
            scene.cam_pos = (fm_vec3_t){ { 0.0f, 0.0f, 200.0f } };
            scene.cam_target = (fm_vec3_t){ { 0.0f, 0.0f, 0.0f } };
            scene.clear_color = RGBA32(8, 8, 12, 255);
        }
        kiln_scene_update(&scene);

        kiln_frame_begin();
          kiln_scene_begin(&scene);
            if (!kiln_splash_done()) kiln_splash_draw3d();
          kiln_gui_begin();
            if (!kiln_splash_done()) {
                kiln_splash_draw2d(SCREEN_W, SCREEN_H);
            } else {
                kiln_gui_text(SCREEN_W / 2 - 76, SCREEN_H / 2,
                             RGBA32(200, 200, 210, 255), "splash done");
            }
          kiln_gui_end();
        kiln_frame_end();
    }
}
