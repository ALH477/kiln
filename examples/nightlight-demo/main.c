// SPDX-License-Identifier: MIT
//
// nightlight-demo: a Sierpinski tetrahedron that wanders and morphs into
// its rotate/invert, kiln_prim so host and console share the picture.
// Two baked instruments per boot (pad + bells), kiln_radio seeded from ticks.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_audio.h>
#include <kiln/kiln_prim.h>
#include <kiln/kiln_radio.h>
#include <kiln/kiln_sierp.h>

#define SCREEN_W 320
#define SCREEN_H 240
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

static const KilnRadioTrack PADS[] = {
    { "rom:/goldberg.wav64",    "after Goldberg Aria",     "J.S. Bach" },
    { "rom:/wiegenlied.wav64",  "after Wiegenlied Op.49",  "J. Brahms" },
    { "rom:/gymnopedie.wav64",  "after Gymnopedie No.1",   "E. Satie" },
    { "rom:/canon.wav64",       "after Canon in D",        "J. Pachelbel" },
};
static const KilnRadioTrack BELLS[] = {
    { "rom:/goldberg_bell.wav64",    "G bells",  "J.S. Bach" },
    { "rom:/wiegenlied_bell.wav64",  "Eb bells", "J. Brahms" },
    { "rom:/gymnopedie_bell.wav64",  "D bells",  "E. Satie" },
    { "rom:/canon_bell.wav64",       "A bells",  "J. Pachelbel" },
};
#define NTRACKS ((int)(sizeof PADS / sizeof PADS[0]))

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);
    kiln_input_init();
    kiln_audio_init((KilnAudioConfig){
        .sample_rate = 32000,
        .latency = 0.16f,
        .sfx_channels = 4,
        .music_channels = 0,
    });

    const uint64_t seed = kiln_radio_mix_seed(get_ticks(), TICKS_READ());
    const int pad_i  = kiln_radio_pick(seed, NTRACKS);
    const int bell_i = kiln_radio_pick(seed ^ 0x9E3779B97F4A7C15ULL, NTRACKS);
    const KilnRadioTrack *pad = &PADS[pad_i];
    const KilnRadioTrack *bell = &BELLS[bell_i];
    int pad_h = kiln_sfx_load(pad->path);
    int bell_h = kiln_sfx_load(bell->path);
    assertf(pad_h >= 0, "nightlight: missing %s", pad->path);
    assertf(bell_h >= 0, "nightlight: missing %s", bell->path);
    kiln_sfx_play_ex(pad_h,  PAD_CH,  1, 0.42f, 0.42f);
    kiln_sfx_play_ex(bell_h, BELL_CH, 1, 0.32f, 0.62f);

    KilnTet root, pose_root[NPOSE];
    kiln_sierp_regular(&root, TET_R);
    pose_root[0] = root;
    kiln_sierp_rotate_y(&pose_root[1], &root, -0.5f, 0.86602540378f); /* 120° */
    kiln_sierp_negate(&pose_root[2], &root);

    static KilnTet pose[NPOSE][64];
    static KilnTet now[64];
    int nleaves = 0;
    for (int p = 0; p < NPOSE; p++) {
        int n = kiln_sierp_leaves(pose[p], 64, &pose_root[p], DEPTH);
        if (n > nleaves) nleaves = n;
    }
    kiln_sierp_morph(now, pose[0], pose[0], nleaves, 0.0f);

    KilnPrim floor, fractal;
    kiln_prim_floor(&floor, 140.0f, 8,
                    kiln_prim_rgba(0x14, 0x16, 0x1C), kiln_prim_rgba(0x10, 0x12, 0x18));
    assertf(kiln_prim_tets(&fractal, now, nleaves) == 0, "nightlight: tet mesh");

    KilnScene scene;
    kiln_scene_init(&scene);
    kiln_prim_stage(&scene, RGBA32(0x08, 0x0A, 0x14, 0xFF), 180.0f, 480.0f);
    scene.fov_deg = 48.0f;
    scene.near_z = 16.0f;
    scene.far_z = 560.0f;

    KilnTransform xf;
    kiln_transform_init(&xf);
    xf.rot_axis = (fm_vec3_t){{ 0.18f, 1.0f, 0.12f }};
    fm_vec3_norm(&xf.rot_axis, &xf.rot_axis);

    int overlay = 0;
    float yaw = 0.0f, t = 0.0f;
    uint32_t frames = 0, last_ticks = get_ticks();
    float fps = 60.0f;

    for (;;) {
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);
        if (in->edges & KILN_BTN_Z) overlay = !overlay;

        t += DT;
        yaw += (DT * 6.2831853f / ORBIT_S) + in->stick_x * 0.03f;
        scene.cam_pos = (fm_vec3_t){{
            fm_sinf(yaw) * RADIUS,
            CAM_Y,
            fm_cosf(yaw) * RADIUS,
        }};
        scene.cam_target = (fm_vec3_t){{ 0, 16, 0 }};
        kiln_scene_update(&scene);

        const float cycle = t / MORPH_S;
        const int i0 = (int)cycle % NPOSE;
        const int i1 = (i0 + 1) % NPOSE;
        const float u = kiln_sierp_smooth(cycle - (float)(int)cycle);
        kiln_sierp_morph(now, pose[i0], pose[i1], nleaves, u);
        kiln_prim_tets_update(&fractal, now, nleaves);

        xf.pos = (fm_vec3_t){{
            26.0f * fm_sinf(t * 0.21f),
            22.0f + 8.0f * fm_sinf(t * 0.33f),
            26.0f * fm_cosf(t * 0.17f),
        }};
        xf.rot_angle = t * 0.41f;

        kiln_frame_begin();
        kiln_scene_begin(&scene);
        kiln_prim_draw(&floor);
        kiln_transform_push(&xf);
        kiln_prim_draw(&fractal);
        kiln_transform_pop();

        kiln_gui_begin();
        if (overlay) {
            if (++frames % 30 == 0) {
                uint32_t now_t = get_ticks();
                fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now_t) / TICKS_PER_SECOND);
                last_ticks = now_t;
            }
            kiln_gui_text(8, 16, RGBA32(0x90, 0x98, 0xB0, 0xFF), "%4.1f fps", fps);
            kiln_gui_text(8, 28, RGBA32(0xC8, 0xD0, 0xE0, 0xFF), "%s", pad->title);
            kiln_gui_text(8, 40, RGBA32(0x70, 0x78, 0x90, 0xFF), "%s + %s",
                          pad->composer, bell->title);
        }
        kiln_gui_end();
        kiln_frame_end();
        kiln_audio_update();
    }
}
