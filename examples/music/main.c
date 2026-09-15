// SPDX-License-Identifier: MIT
//
// Music through kiln_audio: two tracks, and a visualiser of what the mixer is
// actually doing rather than of what it was asked to do.
//
//   1  test.xm64        an XM module on libdragon's XM64 player. libdragon
//                       benchmarks a 10-channel XM at "< 3% CPU and < 10% RSP",
//                       which is why report §5 calls XM64 the pragmatic music
//                       engine for this target.
//   2  cine_loop.wav64  a 15 s looping VADPCM bed (examples/music/synth_loop.py)
//                       on one SFX channel — the other way to ship music.
//
// Every number on screen is read back, not modelled:
//   - the order/row readout is xm64player_tell (kiln_music_tell) — order and
//     row only: its seconds output read 0.0 in Ares while the rows advanced;
//   - the channel pillars light on a NOTE: libxm sets each mixer channel's
//     position every tick, so a retrigger shows as the position jumping back
//     or the channel's waveform changing (mixer_ch_get_pos /
//     mixer_ch_playing_waveform on kiln_music_first_channel + i);
//   - the ring of bars and the scope are the mixed output itself, from
//     kiln_audio's tap — the buffer the AI is about to play.
//
//   A  play / pause         B  stop (rewind)        L / R  track
//   D-pad up / down  volume in 10% steps            idle 4 s: the demo drives
//
// Jump ROMs: .#music-cine boots on track 2 playing, .#music-xm on track 1 with
// the attract tape off — the base ROM's tape changes track after 4 s idle, so
// two captures of it at different times show different TRACKS, not motion.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_audio.h>
#include <kiln/kiln_prim.h>

enum { JUMP_NONE, JUMP_CINE, JUMP_XM };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W 320
#define SCREEN_H 240
#define SAMPLE_RATE 32000

#define BED_CH     0          // the one SFX channel
#define MAX_VIZ_CH 8
#define RING_BARS  24
#define SCOPE_N    96
#define BED_SECS   15.0f

enum { TRACK_XM, TRACK_BED, TRACKS };
enum { ST_STOP, ST_PLAY, ST_PAUSE };
static const char *const ST_NAME[] = { "STOP", "PLAY", "PAUSE" };

static const uint32_t CH_RGB[MAX_VIZ_CH] = {
    0x00F5D4FF, 0xFF4C9AFF, 0xFFD94CFF, 0x6D8BFFFF,
    0xB8F04CFF, 0xFF9E30FF, 0xC04CF0FF, 0x38BDF8FF,
};

// ── the tap: peak since the last frame, and a triggered scope ──────────────
static int16_t g_scope[SCOPE_N];
static int     g_peak_accum;

static void tap(const int16_t *s, int frames, void *ctx)
{
    (void)ctx;
    int start = 0;
    for (int i = 1; i < frames / 2; i++)
        if (s[(i - 1) * 2] < 0 && s[i * 2] >= 0) { start = i; break; }
    for (int i = 0; i < SCOPE_N; i++) {
        int f = start + i * 4;
        if (f >= frames) f = frames - 1;
        g_scope[i] = (int16_t)(((int)s[f * 2] + (int)s[f * 2 + 1]) / 2);
    }
    for (int i = 0; i < frames * 2; i++) {
        const int v = s[i] < 0 ? -s[i] : s[i];
        if (v > g_peak_accum) g_peak_accum = v;
    }
}

// ── attract: volume up, next track, a while, volume down, back ─────────────
static const KilnInputKey ATTRACT_KEYS[] = {
    { .frame =   0 },
    { .frame =  40, .buttons = KILN_BTN_DU }, { .frame =  46 },
    { .frame =  70, .buttons = KILN_BTN_DU }, { .frame =  76 },
    { .frame = 200, .buttons = KILN_BTN_R },  { .frame = 206 },
    { .frame = 520, .buttons = KILN_BTN_DD }, { .frame = 526 },
    { .frame = 560, .buttons = KILN_BTN_DD }, { .frame = 566 },
    { .frame = 600, .buttons = KILN_BTN_L },  { .frame = 606 },
    { .frame = 900, .buttons = KILN_BTN_A },  { .frame = 906 },   // pause
    { .frame = 960, .buttons = KILN_BTN_A },  { .frame = 966 },   // resume
    { .frame = 1100 },
};
static const KilnInputTape ATTRACT = { ATTRACT_KEYS, sizeof ATTRACT_KEYS / sizeof ATTRACT_KEYS[0], 0 };

typedef struct {
    float  energy;
    double last_pos;
    const waveform_t *wave;
    int    hits;
} ChanViz;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static color_t rgb(uint32_t c) { return RGBA32(c >> 24, (c >> 16) & 0xFF, (c >> 8) & 0xFF, 0xFF); }

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    // Mount the ROM's DragonFS before anything opens rom:/.
    dfs_init(DFS_DEFAULT_LOCATION);
    joypad_init();
    kiln_input_init();

    // 1 SFX channel for the bed, 10 for the module (libdragon's XM64 player
    // takes one mixer channel per XM channel).
    kiln_audio_init((KilnAudioConfig){
        .sample_rate = SAMPLE_RATE, .latency = 0.16f, .sfx_channels = 1, .music_channels = 10,
    });
    const int xm  = kiln_music_load("rom:/music/test.xm64");
    const int bed = kiln_sfx_load("rom:/sfx/cine_loop.wav64");
    kiln_audio_set_tap(tap, NULL);

    KilnScene scene;
    kiln_scene_init(&scene);
    kiln_prim_stage(&scene, RGBA32(0x12, 0x10, 0x26, 0xFF), 180.0f, 380.0f);
    scene.fov_deg = 60.0f;
    scene.near_z = 10.0f;
    scene.far_z = 380.0f;

    // ── geometry, built once ──────────────────────────────────────────────
    KilnPrim floor_prim, bar[RING_BARS], pillar_lit[MAX_VIZ_CH], pillar_dim[MAX_VIZ_CH], core;
    kiln_prim_floor(&floor_prim, 200.0f, 16, kiln_prim_rgba(0x44, 0x40, 0x62), kiln_prim_rgba(0x38, 0x34, 0x54));
    for (int i = 0; i < RING_BARS; i++) {
        // Teal to violet to pink round the ring.
        const float t = (float)i / RING_BARS;
        const uint8_t r = (uint8_t)(t < 0.5f ? 0x00 + t * 2 * 0x8B : 0x8B + (t - 0.5f) * 2 * (0xFF - 0x8B));
        const uint8_t g = (uint8_t)(t < 0.5f ? 0xF5 - t * 2 * (0xF5 - 0x5C) : 0x5C - (t - 0.5f) * 2 * (0x5C - 0x4C));
        const uint8_t b = (uint8_t)(t < 0.5f ? 0xD4 + t * 2 * (0xF6 - 0xD4) : 0xF6 - (t - 0.5f) * 2 * (0xF6 - 0x9A));
        const uint32_t c = kiln_prim_rgba(r, g, b);
        kiln_prim_box(&bar[i], (fm_vec3_t){{ 0, 10, 0 }}, (fm_vec3_t){{ 5, 10, 5 }},
                      kiln_prim_shade(c, 1.2f), c, kiln_prim_shade(c, 0.4f));
    }
    for (int i = 0; i < MAX_VIZ_CH; i++) {
        kiln_prim_box(&pillar_lit[i], (fm_vec3_t){{ 0, 10, 0 }}, (fm_vec3_t){{ 6, 10, 6 }},
                      kiln_prim_shade(CH_RGB[i], 1.25f), CH_RGB[i], CH_RGB[i]);
        kiln_prim_box(&pillar_dim[i], (fm_vec3_t){{ 0, 10, 0 }}, (fm_vec3_t){{ 6, 10, 6 }},
                      kiln_prim_shade(CH_RGB[i], 0.55f), kiln_prim_shade(CH_RGB[i], 0.35f),
                      kiln_prim_shade(CH_RGB[i], 0.35f));
    }
    kiln_prim_box(&core, (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 12, 12, 12 }},
                  kiln_prim_rgba(0xFF, 0xE0, 0x60), kiln_prim_rgba(0xFF, 0x98, 0x30), kiln_prim_rgba(0x80, 0x40, 0x10));

    // One transform per drawn object (the RSP reads matrices asynchronously).
    KilnTransform floor_xf, bar_xf[RING_BARS], pillar_xf[MAX_VIZ_CH], core_xf;
    kiln_transform_init(&floor_xf);
    kiln_transform_init(&core_xf);
    for (int i = 0; i < RING_BARS; i++) {
        kiln_transform_init(&bar_xf[i]);
        const float a = (float)i / RING_BARS * 6.2831853f;
        bar_xf[i].pos = (fm_vec3_t){{ fm_cosf(a) * 56.0f, 0, fm_sinf(a) * 56.0f }};
        bar_xf[i].rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
        bar_xf[i].rot_angle = -a;
    }
    for (int i = 0; i < MAX_VIZ_CH; i++) kiln_transform_init(&pillar_xf[i]);

    float  vu[RING_BARS] = { 0 };
    int    vu_head = 0;
    ChanViz viz[MAX_VIZ_CH] = { 0 };
    float  vol[TRACKS] = { 0.7f, 0.7f };
    int    state[TRACKS] = { ST_STOP, ST_STOP };
    double bed_pos = 0.0;
    int    track = TRACK_XM, repeat = 0;

    // Start the chosen track playing, so the ROM makes sound with no pad.
    const int start_track = KILN_JUMP == JUMP_CINE ? TRACK_BED : TRACK_XM;
    track = start_track;
    if (track == TRACK_XM) {
        kiln_music_play(xm);
        kiln_music_set_volume(xm, vol[TRACK_XM]);
    } else {
        kiln_sfx_play_ex(bed, BED_CH, 1, vol[TRACK_BED], 0.5f);
    }
    state[track] = ST_PLAY;
    // A jump ROM holds its state still for the capture; the base ROM demos
    // itself after 4 s of idle.
    if (KILN_JUMP == JUMP_NONE) kiln_input_set_attract(1, &ATTRACT, 240);

    float now = 0.0f;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        const uint32_t ticks = get_ticks();
        now += clampf((float)TICKS_DISTANCE(last_ticks, ticks) / (float)TICKS_PER_SECOND, 0.0f, 0.1f);
        last_ticks = ticks;

        // ── transport ───────────────────────────────────────────────────
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);

        if (in->edges & KILN_BTN_A) {
            if (state[track] == ST_PLAY) {
                // XM: libdragon's stop keeps the cursor, so this IS pause.
                // The bed: remember where it was, since stop forgets.
                if (track == TRACK_XM) kiln_music_stop(xm);
                else { bed_pos = mixer_ch_get_pos(BED_CH); kiln_sfx_stop(BED_CH); }
                state[track] = ST_PAUSE;
            } else {
                if (track == TRACK_XM) {
                    kiln_music_play(xm);
                    kiln_music_set_volume(xm, vol[TRACK_XM]);
                } else {
                    kiln_sfx_play_ex(bed, BED_CH, 1, vol[TRACK_BED], 0.5f);
                    if (state[track] == ST_PAUSE) mixer_ch_set_pos(BED_CH, bed_pos);
                }
                state[track] = ST_PLAY;
            }
        }

        int switch_to = -1;
        if (in->edges & (KILN_BTN_R | KILN_BTN_CR)) switch_to = (track + 1) % TRACKS;
        if (in->edges & (KILN_BTN_L | KILN_BTN_CL)) switch_to = (track + TRACKS - 1) % TRACKS;

        if ((in->edges & KILN_BTN_B) || switch_to >= 0) {
            // Stop means rewind, for both.
            const int was_playing = state[track] == ST_PLAY;
            if (track == TRACK_XM) { kiln_music_stop(xm); kiln_music_seek(xm, 0, 0); }
            else { kiln_sfx_stop(BED_CH); bed_pos = 0.0; }
            state[track] = ST_STOP;
            if (switch_to >= 0) {
                track = switch_to;
                if (was_playing) {
                    if (track == TRACK_XM) { kiln_music_play(xm); kiln_music_set_volume(xm, vol[TRACK_XM]); }
                    else kiln_sfx_play_ex(bed, BED_CH, 1, vol[TRACK_BED], 0.5f);
                    state[track] = ST_PLAY;
                }
            }
        }

        // Volume: a step per press, repeating every 8 frames while held.
        int dv = 0;
        if (in->edges & KILN_BTN_DU) { dv = 1; repeat = 20; }
        else if (in->edges & KILN_BTN_DD) { dv = -1; repeat = 20; }
        else if (in->buttons & (KILN_BTN_DU | KILN_BTN_DD)) {
            if (--repeat <= 0) { dv = (in->buttons & KILN_BTN_DU) ? 1 : -1; repeat = 8; }
        }
        if (dv) {
            // Integer tenths, so ten presses land on exactly 0 or 100%.
            int tenths = (int)(vol[track] * 10.0f + 0.5f) + dv;
            tenths = tenths < 0 ? 0 : tenths > 10 ? 10 : tenths;
            vol[track] = (float)tenths / 10.0f;
            if (track == TRACK_XM) kiln_music_set_volume(xm, vol[track]);
            else if (state[track] == ST_PLAY) kiln_sfx_set_vol_pan(BED_CH, vol[track], 0.5f);
        }

        // ── read the mixer back ─────────────────────────────────────────
        const int first = kiln_music_first_channel(xm);
        int nch = kiln_music_num_channels(xm);
        if (nch > MAX_VIZ_CH) nch = MAX_VIZ_CH;
        for (int i = 0; i < MAX_VIZ_CH; i++) {
            ChanViz *v = &viz[i];
            v->energy *= 0.90f;
            if (track != TRACK_XM || first < 0 || i >= nch) { v->wave = NULL; continue; }
            const int ch = first + i;
            const waveform_t *w = mixer_ch_playing_waveform(ch);
            if (!w) { v->wave = NULL; continue; }
            const double pos = mixer_ch_get_pos(ch);
            if (w != v->wave || pos + 1.0 < v->last_pos) { v->energy = 1.0f; v->hits++; }
            if (v->energy < 0.3f) v->energy = 0.3f;   // sounding, between notes
            v->wave = w;
            v->last_pos = pos;
        }

        const float peak = clampf((float)g_peak_accum / 32768.0f, 0.0f, 1.0f);
        g_peak_accum = 0;
        vu[vu_head] = peak;
        vu_head = (vu_head + 1) % RING_BARS;

        int pat = -1, row = -1;
        kiln_music_tell(xm, &pat, &row, NULL);
        const float bed_secs = state[TRACK_BED] == ST_PLAY ? (float)(mixer_ch_get_pos(BED_CH) / SAMPLE_RATE)
                                                           : (float)(bed_pos / SAMPLE_RATE);

        // ── camera: a slow orbit ────────────────────────────────────────
        const float orbit = now * 0.12f;
        scene.cam_pos = (fm_vec3_t){{ fm_sinf(orbit) * 150.0f, 85.0f, -fm_cosf(orbit) * 150.0f }};
        scene.cam_target = (fm_vec3_t){{ 0, 4, 0 }};
        kiln_scene_update(&scene);

        // ── 3D ──────────────────────────────────────────────────────────
        kiln_frame_begin();
        kiln_scene_begin(&scene);
        kiln_transform_push(&floor_xf); kiln_prim_draw(&floor_prim); kiln_transform_pop();

        // The ring: the newest peak at bar 0, older ones walking round.
        for (int i = 0; i < RING_BARS; i++) {
            const float p = vu[(vu_head + RING_BARS - 1 - i) % RING_BARS];
            // Kept lower than the pillars' reach, so the ring frames them rather
            // than hiding them.
            bar_xf[i].scale = (fm_vec3_t){{ 1.0f, 0.15f + clampf(p * 3.0f, 0.0f, 1.0f) * 1.3f, 1.0f }};
            kiln_transform_push(&bar_xf[i]); kiln_prim_draw(&bar[i]); kiln_transform_pop();
        }

        if (track == TRACK_XM) {
            for (int i = 0; i < nch; i++) {
                const float x = ((float)i - (nch - 1) * 0.5f) * -18.0f;   // ch 1 on the left
                pillar_xf[i].pos = (fm_vec3_t){{ x, 0, 0 }};
                pillar_xf[i].scale = (fm_vec3_t){{ 1.0f, 0.3f + 3.2f * viz[i].energy, 1.0f }};
                kiln_transform_push(&pillar_xf[i]);
                kiln_prim_draw(viz[i].energy > 0.5f ? &pillar_lit[i] : &pillar_dim[i]);
                kiln_transform_pop();
            }
        } else {
            const float s = 0.6f + 0.9f * clampf(peak * 3.0f, 0.0f, 1.0f);
            core_xf.pos = (fm_vec3_t){{ 0, 22, 0 }};
            core_xf.scale = (fm_vec3_t){{ s, s, s }};
            core_xf.rot_axis = (fm_vec3_t){{ 0.3f, 1.0f, 0.2f }};
            fm_vec3_norm(&core_xf.rot_axis, &core_xf.rot_axis);
            core_xf.rot_angle = bed_secs * 0.8f;
            kiln_transform_push(&core_xf); kiln_prim_draw(&core); kiln_transform_pop();
        }

        // ── 2D ──────────────────────────────────────────────────────────
        kiln_gui_begin();
        const color_t ink   = RGBA32(0xE8, 0xE8, 0xF0, 0xFF);
        const color_t dim   = RGBA32(0x90, 0x98, 0xB0, 0xFF);
        const color_t teal  = RGBA32(0x00, 0xF5, 0xD4, 0xFF);
        const color_t panel = RGBA32(0x0C, 0x10, 0x1C, 0xFF);
        const color_t well  = RGBA32(0x2A, 0x2A, 0x3E, 0xFF);
        const color_t st_col = state[track] == ST_PLAY ? teal
                             : state[track] == ST_PAUSE ? RGBA32(0xFF, 0xD9, 0x4C, 0xFF)
                                                        : RGBA32(0xFF, 0x4C, 0x6A, 0xFF);

        kiln_gui_panel(8, 8, 160, 48, panel, teal);
        kiln_gui_text(14, 21, teal, "KILN MUSIC");
        kiln_gui_text(96, 21, st_col, "%s", ST_NAME[state[track]]);
        kiln_gui_text(14, 33, ink, "%d/2 %s", track + 1, track == TRACK_XM ? "test.xm64" : "cine_loop.wav64");
        kiln_gui_text(14, 45, dim, "vol %3d%%", (int)(vol[track] * 100.0f + 0.5f));
        kiln_gui_bar(76, 39, 84, 6, vol[track], teal, well);

        const int rx = SCREEN_W - 136;
        kiln_gui_panel(rx, 8, 128, 48, panel, RGBA32(0x8B, 0x5C, 0xF6, 0xFF));
        if (track == TRACK_XM) {
            kiln_gui_text(rx + 6, 21, ink, "ord %02d row %02d", pat < 0 ? 0 : pat, row < 0 ? 0 : row);
            // No clock: xm64player_tell's seconds stayed 0.0 in Ares while its
            // order and row advanced, so a time here would be a wrong number.
            int sounding = 0;
            for (int i = 0; i < nch; i++) sounding += viz[i].wave != NULL;
            kiln_gui_text(rx + 6, 33, dim, "%d ch  %d sounding", kiln_music_num_channels(xm), sounding);
            for (int i = 0; i < nch; i++) {
                const int bx = rx + 6 + i * 14;
                kiln_gui_rect(bx, 38, 10, 14, well);
                const int h = (int)(viz[i].energy * 12.0f);
                kiln_gui_rect(bx + 1, 51 - h, 8, h, rgb(CH_RGB[i]));
            }
        } else {
            kiln_gui_text(rx + 6, 21, ink, "VADPCM loop, 1 ch");
            kiln_gui_text(rx + 6, 33, dim, "%4.1f / %4.1f s", (double)bed_secs, (double)BED_SECS);
            kiln_gui_bar(rx + 6, 42, 116, 6, bed_secs / BED_SECS, RGBA32(0xFF, 0xD9, 0x4C, 0xFF), well);
        }

        if (kiln_input_scripted(1)) {
            kiln_gui_panel(SCREEN_W - 58, 62, 50, 16, RGBA32(0xC0, 0x30, 0x60, 0xFF), RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
            kiln_gui_text(SCREEN_W - 49, 74, RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "DEMO");
        }

        // The mixed output, as the AI will play it.
        const int sy = SCREEN_H - 62;
        kiln_gui_panel(8, sy, SCREEN_W - 16, 36, panel, RGBA32(0x3A, 0x40, 0x5C, 0xFF));
        kiln_gui_text(14, sy + 13, dim, "mix");
        kiln_gui_bar(14, sy + 20, 24, 6, peak, RGBA32(0xFF, 0xD9, 0x4C, 0xFF), well);
        const int mid = sy + 18;
        for (int i = 1; i < SCOPE_N; i++) {
            const int x0 = 48 + (i - 1) * 256 / SCOPE_N, x1 = 48 + i * 256 / SCOPE_N;
            const int y0 = mid - (int)clampf(g_scope[i - 1] / 900.0f, -14, 14);
            const int y1 = mid - (int)clampf(g_scope[i] / 900.0f, -14, 14);
            kiln_gui_line(x0, y0, x1, y1, 1, teal);
        }

        kiln_gui_panel(8, SCREEN_H - 24, SCREEN_W - 16, 16, panel, RGBA32(0x8B, 0x5C, 0xF6, 0xFF));
        kiln_gui_text(14, SCREEN_H - 12, ink, "A play  B stop  L/R track  up/dn vol");

        kiln_gui_end();
        kiln_frame_end();

        kiln_audio_update();
    }
}
