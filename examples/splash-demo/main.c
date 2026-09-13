// SPDX-License-Identifier: MIT
//
// The Kiln boot splash, and then something to look at. The engine's publisher
// mark plays with its jingle — the brick kiln spins to rest on the beat, the
// flame flickers in its doorway, the publisher line fades up — and hands over
// to a lit, fogged turntable of the same model on a stone pedestal, with
// embers rising from the chimney and the credits underneath.
//
//   kiln_splash  -> the real four-call sequence from kiln_splash.h, untouched
//   kiln_audio   -> rom:/kilnjingle.wav64, whose chord lands on the beat
//   kiln_prim    -> the pedestal, the courtyard floor and the embers
//   kiln_input   -> any button skips the splash; START on the stage replays it
//
// The stage replays the splash by itself every STAGE_HOLD seconds, so a ROM
// left running shows both halves. The flame is drawn under its own transform
// with its own flicker, the same way kiln_splash_draw3d does it, so it stays
// alive after the splash has finished.
//
// Jump ROM: .#splash-demo-stage boots straight onto the turntable and never
// auto-replays, so a capture at any time shows the stage.

#include <libdragon.h>
#include <t3d/t3dmodel.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_audio.h>
#include <kiln/kiln_splash.h>
#include <kiln/kiln_prim.h>

#include <string.h>

enum { JUMP_NONE, JUMP_STAGE };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W 320
#define SCREEN_H 240

#define STAGE_HOLD 20.0f   /* seconds on the turntable before the splash replays */
#define SPIN_RATE  0.55f   /* turntable, radians per second                       */
#define EMBERS     6
#define PEDESTAL_H 20.0f   /* the logo stands on top of this                      */
#define CHIMNEY_Y  (PEDESTAL_H + 126.0f)

static const char *const PUBLISHER = "Kiln Engine - MIT Licensed";

typedef enum { ST_SPLASH, ST_STAGE } State;

/* Same shape as kiln_splash.c's flicker: two incommensurate sines, so the
 * flame breathes rather than pulsing like a metronome. */
static float flicker(float t)
{
    return 1.0f + 0.10f * fm_sinf(t * 26.0f) + 0.06f * fm_sinf(t * 41.0f + 1.7f);
}

static void text_centred(int y, color_t c, const char *s)
{
    kiln_gui_text(SCREEN_W / 2 - (int)strlen(s) * 3, y, c, "%s", s);
}

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);   /* also initialises kiln_gui */
    joypad_init();
    /* Mount DragonFS before the first rom:/ open. The host resolves rom:/
     * without it; the console asserts "File not found" instead. */
    dfs_init(DFS_DEFAULT_LOCATION);
    kiln_input_init();
    kiln_audio_init(KILN_AUDIO_DEFAULT);

    /* Uncompressed on purpose (flake.nix's kilnLogoRaw): the host's .t3dm
     * reader has no decompression stage, and the pc build ships this file. */
    T3DModel *model = t3d_model_load("rom:/models/kiln_logo.t3dm");
    int jingle = kiln_sfx_load("rom:/kilnjingle.wav64");

    T3DObject *obj_kiln = model ? t3d_model_get_object(model, "kiln") : NULL;
    T3DObject *obj_flame = model ? t3d_model_get_object(model, "flame") : NULL;
    T3DObject *obj_plate = model ? t3d_model_get_object(model, "plate") : NULL;

    /* ── The stage ─────────────────────────────────────────────────────
     * Two scenes, each initialised once: the splash owns its camera and a
     * black clear, and must not inherit the stage's fog and lights. */
    KilnScene splash_scene;
    kiln_scene_init(&splash_scene);

    KilnScene stage;
    kiln_scene_init(&stage);
    /* Dusk-orange and bright on purpose. The first Ares capture, at a dark
     * brown sky and mid-tone flagstones, came out near-black: flat faces
     * read at roughly their ambient share on console, so the ambient is
     * raised over kiln_prim_stage's preset as well. */
    kiln_prim_stage(&stage, RGBA32(0x8A, 0x5C, 0x48, 0xFF), 460.0f, 1000.0f);
    stage.ambient[0] = 0x90; stage.ambient[1] = 0x78; stage.ambient[2] = 0x6C;
    stage.fov_deg = 60.0f;
    stage.near_z = 24.0f;
    stage.far_z = 1000.0f;

    KilnPrim floor_prim, pedestal_prim, ember_prim;
    kiln_prim_floor(&floor_prim, 640.0f, 16,
                    kiln_prim_rgba(0xE0, 0xC8, 0xB0), kiln_prim_rgba(0xC0, 0xA4, 0x8C));
    kiln_prim_box(&pedestal_prim, (fm_vec3_t){{ 0, PEDESTAL_H * 0.5f, 0 }},
                  (fm_vec3_t){{ 118, PEDESTAL_H * 0.5f, 118 }},
                  kiln_prim_rgba(0xD8, 0xCC, 0xBC), kiln_prim_rgba(0x9C, 0x8C, 0x7C),
                  kiln_prim_rgba(0x40, 0x38, 0x30));
    /* Embers are lit like everything else, so they are built near-white-hot
     * to still read orange once the key light and the fog have had them. */
    kiln_prim_box(&ember_prim, (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 5, 5, 5 }},
                  kiln_prim_rgba(0xFF, 0xF4, 0xB0), kiln_prim_rgba(0xFF, 0xB0, 0x40),
                  kiln_prim_rgba(0xFF, 0x70, 0x20));

    /* One transform per drawn object: the RSP reads a matrix after this
     * frame's CPU work has moved on, so one buffer rewritten between two draws
     * would put both draws wherever it was left. */
    KilnTransform logo_xf, flame_xf, pedestal_xf, ember_xf[EMBERS];
    kiln_transform_init(&logo_xf);
    kiln_transform_init(&flame_xf);
    kiln_transform_init(&pedestal_xf);
    for (int i = 0; i < EMBERS; i++) kiln_transform_init(&ember_xf[i]);

    State state = ST_SPLASH;
    if (KILN_JUMP == JUMP_STAGE) state = ST_STAGE;
    else kiln_splash_init(model, jingle, PUBLISHER);

    float stage_t = 0.0f;   /* seconds since the stage began                */
    float clock = 0.0f;     /* never resets: flicker and embers run on this */
    int replays = 0;

    for (;;) {
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);
        const float dt = 1.0f / 60.0f;
        clock += dt;

        if (state == ST_SPLASH) {
            kiln_splash_update(dt, in);
            if (kiln_splash_done()) {
                state = ST_STAGE;
                stage_t = 0.0f;
            }
        } else {
            stage_t += dt;
            const int auto_replay = KILN_JUMP != JUMP_STAGE && stage_t >= STAGE_HOLD;
            if ((in->edges & KILN_BTN_START) || auto_replay) {
                /* Re-arming is the whole replay: kiln_splash_init resets its
                 * clock, its flash and its jingle channel. This frame's START
                 * edge is not passed to the splash, or it would skip itself. */
                kiln_splash_init(model, jingle, PUBLISHER);
                state = ST_SPLASH;
                replays++;
            }
        }

        kiln_frame_begin();

        if (state == ST_SPLASH) {
            kiln_splash_apply(&splash_scene);
            kiln_scene_update(&splash_scene);
            kiln_scene_begin(&splash_scene);
            kiln_splash_draw3d();
            kiln_gui_begin();
            kiln_splash_draw2d(SCREEN_W, SCREEN_H);
            kiln_gui_end();
        } else {
            /* A slow drift around the front, so the pedestal's edge and the
             * floor's checker move against each other and the depth reads. */
            const float sway = fm_sinf(clock * 0.23f);
            stage.cam_pos = (fm_vec3_t){{ sway * 120.0f, 170.0f, 500.0f }};
            stage.cam_target = (fm_vec3_t){{ sway * 24.0f, 104.0f, 0.0f }};
            kiln_scene_update(&stage);
            kiln_scene_begin(&stage);

            kiln_prim_draw(&floor_prim);

            const float spin = clock * SPIN_RATE;
            pedestal_xf.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
            pedestal_xf.rot_angle = spin;
            kiln_transform_push(&pedestal_xf);
            kiln_prim_draw(&pedestal_prim);
            kiln_transform_pop();

            logo_xf.pos = (fm_vec3_t){{ 0, PEDESTAL_H, 0 }};
            logo_xf.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
            logo_xf.rot_angle = spin;
            if (obj_kiln || obj_flame || obj_plate) {
                kiln_transform_push(&logo_xf);
                if (obj_kiln) t3d_model_draw_object(obj_kiln, NULL);
                if (obj_plate) t3d_model_draw_object(obj_plate, NULL);
                kiln_transform_pop();
                if (obj_flame) {
                    const float f = flicker(clock);
                    flame_xf.pos = logo_xf.pos;
                    flame_xf.rot_axis = logo_xf.rot_axis;
                    flame_xf.rot_angle = spin;
                    flame_xf.scale = (fm_vec3_t){{ f, 0.92f + 0.08f * f, f }};
                    kiln_transform_push(&flame_xf);
                    t3d_model_draw_object(obj_flame, NULL);
                    kiln_transform_pop();
                }
            } else if (model) {
                kiln_transform_push(&logo_xf);
                t3d_model_draw(model);
                kiln_transform_pop();
            }

            /* Embers: each rises out of the chimney on its own phase and
             * drifts outward, shrinking as it goes, then starts again. */
            for (int i = 0; i < EMBERS; i++) {
                const float ph = clock * 0.42f + (float)i / EMBERS;
                const float u = ph - (float)(int)ph;            /* 0..1 per trip */
                const float a = (float)i * 2.39996f + u * 1.3f;   /* golden angle  */
                const float r = 8.0f + u * 46.0f;
                const float s = 1.0f - 0.8f * u;
                ember_xf[i].pos = (fm_vec3_t){{ fm_cosf(a) * r, CHIMNEY_Y + u * 150.0f,
                                                fm_sinf(a) * r }};
                ember_xf[i].scale = (fm_vec3_t){{ s, s, s }};
                ember_xf[i].rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
                ember_xf[i].rot_angle = u * 6.0f;
                kiln_transform_push(&ember_xf[i]);
                kiln_prim_draw(&ember_prim);
                kiln_transform_pop();
            }

            kiln_gui_begin();
            const color_t ink = RGBA32(0xF0, 0xE6, 0xDC, 0xFF);
            const color_t ember = RGBA32(0xFF, 0x9A, 0x3C, 0xFF);
            const color_t dim = RGBA32(0xB8, 0xA8, 0x98, 0xFF);

            kiln_gui_text(12, 20, ember, "KILN");
            kiln_gui_text(12, 32, dim, "turntable");
            kiln_gui_text(SCREEN_W - 12 - 18 * 6, 20, dim, "libdragon + Tiny3D");

            text_centred(SCREEN_H - 34, ink, PUBLISHER);

            kiln_gui_panel(8, SCREEN_H - 26, SCREEN_W - 16, 18,
                           RGBA32(0x20, 0x14, 0x12, 0xFF), ember);
            kiln_gui_text(16, SCREEN_H - 13, ink, "START replay the splash");
            if (KILN_JUMP != JUMP_STAGE) {
                int left = (int)(STAGE_HOLD - stage_t + 0.99f);
                if (left < 0) left = 0;
                kiln_gui_text(SCREEN_W - 70, SCREEN_H - 13, dim, "auto %2ds", left);
            } else {
                kiln_gui_text(SCREEN_W - 70, SCREEN_H - 13, dim, "held");
            }
            if (replays > 0) kiln_gui_text(SCREEN_W - 12 - 8 * 6, 32, dim, "replay %d", replays);
            kiln_gui_end();
        }

        kiln_frame_end();
        kiln_audio_update();
    }
}
