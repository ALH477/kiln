// SPDX-License-Identifier: MIT
//
// nightlight-demo: a Sierpinski tetrahedron that wanders and morphs into
// its rotate/invert, fig_prim so host and console share the picture.
// Two baked instruments (pad + bells). Boot shuffles; Z opens a menu to
// pick, A/B skip while the menu is closed.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_audio.h>
#include <kiln/kiln_prim.h>
#include <kiln/kiln_radio.h>
#include <kiln/kiln_sierp.h>

#define DT       (1.0f / 60.0f)
#define ORBIT_S  70.0f
#define RADIUS   220.0f
#define CAM_Y    70.0f
#define DEPTH    3
#define TET_R    56.0f
#define MORPH_S  11.0f
#define PAD_CH   0
#define BELL_CH  1
#define NPOSE    3

static const FigRadioTrack PADS[] = {
    { "rom:/goldberg.wav64",    "after Goldberg Aria",     "J.S. Bach" },
    { "rom:/wiegenlied.wav64",  "after Wiegenlied Op.49",  "J. Brahms" },
    { "rom:/gymnopedie.wav64",  "after Gymnopedie No.1",   "E. Satie" },
    { "rom:/canon.wav64",       "after Canon in D",        "J. Pachelbel" },
};
static const FigRadioTrack BELLS[] = {
    { "rom:/goldberg_bell.wav64",    "G bells",  "J.S. Bach" },
    { "rom:/wiegenlied_bell.wav64",  "Eb bells", "J. Brahms" },
    { "rom:/gymnopedie_bell.wav64",  "D bells",  "E. Satie" },
    { "rom:/canon_bell.wav64",       "A bells",  "J. Pachelbel" },
};
#define NTRACKS ((int)(sizeof PADS / sizeof PADS[0]))

static int wrap(int i, int n, int d)
{
    i = (i + d) % n;
    return i < 0 ? i + n : i;
}

static void play_pad(int handles[NTRACKS], int i)
{
    fig_sfx_stop(PAD_CH);
    fig_sfx_play_ex(handles[i], PAD_CH, 1, 0.42f, 0.42f);
}

static void play_bells(int handles[NTRACKS], int i)
{
    fig_sfx_stop(BELL_CH);
    fig_sfx_play_ex(handles[i], BELL_CH, 1, 0.32f, 0.62f);
}

int main(void)
{
    fig_engine_init(RESOLUTION_320x240);
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);
    fig_input_init();
    fig_audio_init((FigAudioConfig){
        .sample_rate = 32000,
        .latency = 0.16f,
        .sfx_channels = 4,
        .music_channels = 0,
    });

    int pad_h[NTRACKS], bell_h[NTRACKS];
    for (int i = 0; i < NTRACKS; i++) {
        pad_h[i] = fig_sfx_load(PADS[i].path);
        bell_h[i] = fig_sfx_load(BELLS[i].path);
        assertf(pad_h[i] >= 0, "nightlight: missing %s", PADS[i].path);
        assertf(bell_h[i] >= 0, "nightlight: missing %s", BELLS[i].path);
    }

    const uint64_t seed = fig_radio_mix_seed(get_ticks(), TICKS_READ());
    int pad_i  = fig_radio_pick(seed, NTRACKS);
    int bell_i = fig_radio_pick(seed ^ 0x9E3779B97F4A7C15ULL, NTRACKS);
    play_pad(pad_h, pad_i);
    play_bells(bell_h, bell_i);

    FigTet root, pose_root[NPOSE];
    fig_sierp_regular(&root, TET_R);
    pose_root[0] = root;
    fig_sierp_rotate_y(&pose_root[1], &root, -0.5f, 0.86602540378f); /* 120° */
    fig_sierp_negate(&pose_root[2], &root);

    static FigTet pose[NPOSE][64];
    static FigTet now[64];
    int nleaves = 0;
    for (int p = 0; p < NPOSE; p++) {
        int n = fig_sierp_leaves(pose[p], 64, &pose_root[p], DEPTH);
        if (n > nleaves) nleaves = n;
    }
    fig_sierp_morph(now, pose[0], pose[0], nleaves, 0.0f);

    FigPrim floor, fractal;
    fig_prim_floor(&floor, 140.0f, 8,
                    fig_prim_rgba(0x14, 0x16, 0x1C), fig_prim_rgba(0x10, 0x12, 0x18));
    assertf(fig_prim_tets(&fractal, now, nleaves) == 0, "nightlight: tet mesh");

    FigScene scene;
    fig_scene_init(&scene);
    fig_prim_stage(&scene, RGBA32(0x08, 0x0A, 0x14, 0xFF), 180.0f, 480.0f);
    scene.fov_deg = 48.0f;
    scene.near_z = 16.0f;
    scene.far_z = 560.0f;

    FigTransform xf;
    fig_transform_init(&xf);
    xf.rot_axis = (fm_vec3_t){{ 0.18f, 1.0f, 0.12f }};
    fm_vec3_norm(&xf.rot_axis, &xf.rot_axis);

    int menu = 0;
    int col = 0;   /* 0 pad, 1 bells */
    int row = pad_i;
    float yaw = 0.0f, t = 0.0f;

    for (;;) {
        fig_input_update();
        const FigInput *in = fig_input_get(1);

        if (in->edges & (FIG_BTN_Z | FIG_BTN_START)) {
            menu = !menu;
            if (menu) {
                col = 0;
                row = pad_i;
            }
        }

        if (menu) {
            if (in->edges & FIG_BTN_DL) { col = 0; row = pad_i; }
            if (in->edges & FIG_BTN_DR) { col = 1; row = bell_i; }
            if (in->edges & FIG_BTN_DU) row = wrap(row, NTRACKS, -1);
            if (in->edges & FIG_BTN_DD) row = wrap(row, NTRACKS, +1);
            if (in->edges & FIG_BTN_A) {
                if (col == 0) { pad_i = row; play_pad(pad_h, pad_i); }
                else          { bell_i = row; play_bells(bell_h, bell_i); }
            }
        } else {
            if (in->edges & (FIG_BTN_A | FIG_BTN_R | FIG_BTN_CR)) {
                pad_i = wrap(pad_i, NTRACKS, +1);
                play_pad(pad_h, pad_i);
            }
            if (in->edges & (FIG_BTN_L | FIG_BTN_CL)) {
                pad_i = wrap(pad_i, NTRACKS, -1);
                play_pad(pad_h, pad_i);
            }
            if (in->edges & FIG_BTN_B) {
                bell_i = wrap(bell_i, NTRACKS, +1);
                play_bells(bell_h, bell_i);
            }
        }

        t += DT;
        if (!menu)
            yaw += (DT * 6.2831853f / ORBIT_S) + in->stick_x * 0.03f;
        else
            yaw += DT * 6.2831853f / ORBIT_S;
        scene.cam_pos = (fm_vec3_t){{
            fm_sinf(yaw) * RADIUS,
            CAM_Y,
            fm_cosf(yaw) * RADIUS,
        }};
        scene.cam_target = (fm_vec3_t){{ 0, 16, 0 }};
        fig_scene_update(&scene);

        const float cycle = t / MORPH_S;
        const int i0 = (int)cycle % NPOSE;
        const int i1 = (i0 + 1) % NPOSE;
        const float u = fig_sierp_smooth(cycle - (float)(int)cycle);
        fig_sierp_morph(now, pose[i0], pose[i1], nleaves, u);
        fig_prim_tets_update(&fractal, now, nleaves);

        xf.pos = (fm_vec3_t){{
            26.0f * fm_sinf(t * 0.21f),
            22.0f + 8.0f * fm_sinf(t * 0.33f),
            26.0f * fm_cosf(t * 0.17f),
        }};
        xf.rot_angle = t * 0.41f;

        fig_frame_begin();
        fig_scene_begin(&scene);
        fig_prim_draw(&floor);
        fig_transform_push(&xf);
        fig_prim_draw(&fractal);
        fig_transform_pop();

        fig_gui_begin();
        if (menu) {
            fig_gui_panel(6, 8, 308, 224,
                            RGBA32(0x08, 0x0A, 0x14, 0xD0),
                            RGBA32(0x58, 0x60, 0x78, 0xFF));
            fig_gui_text(16, 18, RGBA32(0xC8, 0xD0, 0xE0, 0xFF), "nightlight");
            fig_gui_text(16, 32, RGBA32(0x70, 0x78, 0x90, 0xFF),
                          "D-pad  A play  Z close");
            fig_gui_text(16, 52, RGBA32(col == 0 ? 0xFF : 0x90,
                                         col == 0 ? 0xC8 : 0x98,
                                         col == 0 ? 0x90 : 0xB0, 0xFF),
                          "PAD");
            fig_gui_text(168, 52, RGBA32(col == 1 ? 0xFF : 0x90,
                                          col == 1 ? 0xC8 : 0x98,
                                          col == 1 ? 0x90 : 0xB0, 0xFF),
                          "BELLS");
            for (int i = 0; i < NTRACKS; i++) {
                const int y = 68 + i * 14;
                const int pad_here = (col == 0 && i == row);
                const int bell_here = (col == 1 && i == row);
                fig_gui_text(16, y,
                              pad_here ? RGBA32(0xFF, 0xE0, 0xB0, 0xFF)
                                       : (i == pad_i ? RGBA32(0xC8, 0xD0, 0xE0, 0xFF)
                                                     : RGBA32(0x58, 0x60, 0x78, 0xFF)),
                              "%s%s", pad_here ? "> " : "  ", PADS[i].title);
                fig_gui_text(168, y,
                              bell_here ? RGBA32(0xFF, 0xE0, 0xB0, 0xFF)
                                        : (i == bell_i ? RGBA32(0xC8, 0xD0, 0xE0, 0xFF)
                                                       : RGBA32(0x58, 0x60, 0x78, 0xFF)),
                              "%s%s", bell_here ? "> " : "  ", BELLS[i].title);
            }
            fig_gui_text(16, 140, RGBA32(0x70, 0x78, 0x90, 0xFF),
                          "now  %s", PADS[pad_i].title);
            fig_gui_text(16, 154, RGBA32(0x70, 0x78, 0x90, 0xFF),
                          "     %s + %s", PADS[pad_i].composer, BELLS[bell_i].title);
            fig_gui_text(16, 180, RGBA32(0x58, 0x60, 0x78, 0xFF),
                          "closed: A/R next pad  B next bells");
        }
        fig_gui_end();
        fig_frame_end();
        fig_audio_update();
    }
}
