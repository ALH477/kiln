// SPDX-License-Identifier: MIT
//
// One string, two ways, side by side: dsp/ks.dsp synthesised LIVE on the
// VR4300 and the same .dsp BAKED offline and played by the RSP mixer.
//
//   live   ks-voice: `faust -lang c -single -os`, linked into this ROM, one
//          frame() call per sample, rendered into a scratch buffer and summed
//          by hand into the AI buffer after mixer_poll.
//   baked  ks-baked: rendered at full double precision on the host, VADPCM
//          encoded, played on one mixer channel and retuned with
//          kiln_sfx_set_pitch.
//
// The report's hybrid rule is bake 80-90% and keep live synthesis for what
// must be parametric; this is the A/B that rule is argued from. Levels are
// matched (see LIVE_GAIN) so the difference is precision and synthesis, not
// volume.
//
// What is on screen is what was computed, not a model of it: the two scopes
// and the two 3D strings are the samples of the last buffer — the CPU's
// scratch buffer and the RSP's mix before the live voice is added — and the
// millisecond figure is get_ticks around the live render.
//
//   stick / D-pad / C-left,right  pick a note      A  live      B  baked
//   Z  both at once                                 idle 3 s: the demo plays
//
// Jump ROM: .#live-voice-ab plucks live and baked alternately, forever.
//
// ── Two bugs this used to have ───────────────────────────────────────────
// The trigger was a gate zone set in the input block and cleared at the END
// of the frame. The live voice only renders inside `while (audio_can_write())`,
// so a press on a frame with no buffer to fill set the gate and cleared it
// again with no sample ever seeing it: the press was silently dropped. Now a
// press is queued and consumed by the render loop, and the gate is held for
// exactly GATE_FRAMES samples — 20 ms, the same pulse mkBakedInstrument
// renders the baked asset with — split across buffers as it falls.
//
// The render length was clamped to a 1024-frame scratch while
// audio_get_buffer_length() is what mixer_poll fills. At 32 kHz libdragon's
// buffer is 640 frames (CALC_BUFFER: rate/50 rounded up to 16), so the clamp
// never bit — but at 64 kHz it would have left the tail of every buffer
// without the live voice. The scratch is now sized from the real length at
// boot.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_audio.h>
#include <kiln/kiln_prim.h>

#include <malloc.h>

enum { JUMP_NONE, JUMP_AB };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

// The Faust voice API, emitted by dsp/arch/libdragon_mixer.c with -cn ksvoice.
float *faust_n64_ksvoice_param(const char *label);
void   faust_n64_ksvoice_init(int sample_rate);
void   faust_n64_ksvoice_set_gain(float gain);
void   faust_n64_ksvoice_render(int16_t *out, int nframes, int accumulate);

#define SCREEN_W 320
#define SCREEN_H 240

// Must match both mkFaustVoice's and mkBakedInstrument's sampleRate.
#define SAMPLE_RATE 32000
#define BAKED_HZ    220.0f   // ks-baked's `freq`
#define BAKED_SECS  2.0f
#define CH_BAKED    0
#define GATE_FRAMES 640      // 20 ms: ks-baked's gate = { on = 0.0; off = 0.02; }

// Level match: ks-baked is rendered with gain 0.25 and kiln_sfx's centre pan
// sends half of that to each side; the live voice writes both sides at
// gain-zone x set_gain. 0.25 x 0.5 is the same 0.125 per side.
#define LIVE_GAIN_ZONE 0.25f
#define LIVE_SET_GAIN  0.5f

#define NOTES   7
#define SCOPE_N 96
#define STRING_N 24
#define LIVE_Y  46.0f        // the two strings' rest heights
#define BAKED_Y 16.0f

// A minor pentatonic, A3..C5. Frequencies in Hz for the live voice; the
// baked voice uses the same numbers as ratios of BAKED_HZ.
static const float NOTE_HZ[NOTES] = { 220.00f, 261.63f, 293.66f, 329.63f, 392.00f, 440.00f, 523.25f };
static const char *const NOTE_NAME[NOTES] = { "A3", "C4", "D4", "E4", "G4", "A4", "C5" };

// ── audio state, touched only by the pump ──────────────────────────────────
static int16_t *g_scratch;
static int      g_scratch_frames;
static float   *g_freq_zone, *g_gate_zone;
static int      g_live_pending;     // presses not yet rendered
static float    g_live_pending_hz;
static int      g_gate_left;        // frames of gate still to render
static float    g_render_ms;        // last live render, per buffer

typedef struct { int16_t s[SCOPE_N]; int peak; } Scope;
static Scope g_scope_live, g_scope_baked;

static void capture(const int16_t *s, int frames, Scope *out)
{
    int start = 0;
    for (int i = 1; i < frames / 2; i++)
        if (s[(i - 1) * 2] < 0 && s[i * 2] >= 0) { start = i; break; }
    for (int i = 0; i < SCOPE_N; i++) {
        int f = start + i * 3;
        if (f >= frames) f = frames - 1;
        out->s[i] = (int16_t)(((int)s[f * 2] + (int)s[f * 2 + 1]) / 2);
    }
    int peak = 0;
    for (int i = 0; i < frames * 2; i++) {
        const int v = s[i] < 0 ? -s[i] : s[i];
        if (v > peak) peak = v;
    }
    out->peak = peak;
}

// Render `n` frames of the live voice into g_scratch, applying queued presses
// with a gate of exactly GATE_FRAMES wherever it lands.
static void render_live(int n)
{
    const uint32_t t0 = get_ticks();
    int done = 0;
    while (done < n) {
        if (g_gate_left == 0 && g_live_pending > 0) {
            g_live_pending--;
            if (g_freq_zone) *g_freq_zone = g_live_pending_hz;
            if (g_gate_zone) *g_gate_zone = 1.0f;
            g_gate_left = GATE_FRAMES;
        }
        int chunk = n - done;
        if (g_gate_left > 0 && chunk > g_gate_left) chunk = g_gate_left;
        faust_n64_ksvoice_render(g_scratch + done * 2, chunk, 0);
        done += chunk;
        if (g_gate_left > 0) {
            g_gate_left -= chunk;
            // Falling edge: ks.dsp plucks on the RISING edge, so the gate has
            // to come down before the next press can pluck again.
            if (g_gate_left == 0 && g_gate_zone) *g_gate_zone = 0.0f;
        }
    }
    g_render_ms = (float)TICKS_DISTANCE(t0, get_ticks()) * 1000.0f / (float)TICKS_PER_SECOND;
}

static void audio_pump(void)
{
    while (audio_can_write()) {
        short *buf = audio_write_begin();
        const int n = audio_get_buffer_length();
        assertf(n <= g_scratch_frames, "AI buffer grew to %d frames, scratch is %d", n, g_scratch_frames);

        // RSP: the baked channel, at high priority (see kiln_audio_update).
        rspq_highpri_begin();
        mixer_poll(buf, n);
        rspq_highpri_end();
        capture(buf, n, &g_scope_baked);

        // VR4300: the live voice, then a saturating sum on top.
        render_live(n);
        capture(g_scratch, n, &g_scope_live);
        for (int i = 0; i < n * 2; i++) {
            const int32_t s = (int32_t)buf[i] + (int32_t)g_scratch[i];
            buf[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        }
        audio_write_end();
    }
}

// ── attract: live, baked, next note, both, and back down ───────────────────
static const KilnInputKey ATTRACT_KEYS[] = {
    { .frame =   0 },
    { .frame =  20, .buttons = KILN_BTN_A },  { .frame =  26 },
    { .frame = 110, .buttons = KILN_BTN_B },  { .frame = 116 },
    { .frame = 200, .sx = 70 },               { .frame = 206 },
    { .frame = 220, .buttons = KILN_BTN_A },  { .frame = 226 },
    { .frame = 300, .buttons = KILN_BTN_B },  { .frame = 306 },
    { .frame = 380, .sx = 70 },               { .frame = 386 },
    { .frame = 400, .buttons = KILN_BTN_Z },  { .frame = 406 },
    { .frame = 500, .sx = 70 },               { .frame = 506 },
    { .frame = 520, .buttons = KILN_BTN_A },  { .frame = 526 },
    { .frame = 600, .buttons = KILN_BTN_B },  { .frame = 606 },
    { .frame = 680, .sx = -70 },              { .frame = 720 },
    { .frame = 760 },
};
static const KilnInputTape ATTRACT = { ATTRACT_KEYS, sizeof ATTRACT_KEYS / sizeof ATTRACT_KEYS[0], 0 };

// Jump ROM .#live-voice-ab: live and baked plucked alternately forever, 20
// frames apart, so a capture at any moment has both strings ringing.
static const KilnInputKey AB_KEYS[] = {
    { .frame =  0, .buttons = KILN_BTN_A }, { .frame =  4 },
    { .frame = 20, .buttons = KILN_BTN_B }, { .frame = 24 },
    { .frame = 40 },
};
static const KilnInputTape AB_TAPE = { AB_KEYS, sizeof AB_KEYS / sizeof AB_KEYS[0], 0 };

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void draw_scope(int x, int y, int w, const Scope *sc, const char *label, color_t col)
{
    const color_t panel = RGBA32(0x0C, 0x10, 0x1C, 0xFF);
    kiln_gui_panel(x, y, w, 38, panel, col);
    kiln_gui_text(x + 6, y + 12, col, "%s", label);
    kiln_gui_bar(x + w - 44, y + 6, 38, 5, sc->peak / 32768.0f, col, RGBA32(0x2A, 0x2A, 0x3E, 0xFF));
    const int mid = y + 25;
    for (int i = 1; i < SCOPE_N; i++) {
        const int x0 = x + 4 + (i - 1) * (w - 8) / SCOPE_N, x1 = x + 4 + i * (w - 8) / SCOPE_N;
        const int y0 = mid - (int)clampf(sc->s[i - 1] / 500.0f, -11, 11);
        const int y1 = mid - (int)clampf(sc->s[i] / 500.0f, -11, 11);
        kiln_gui_line(x0, y0, x1, y1, 1, col);
    }
}

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    // Mount the ROM's DragonFS before anything opens rom:/.
    dfs_init(DFS_DEFAULT_LOCATION);
    joypad_init();
    kiln_input_init();

    // One mixer channel, for the baked voice (baked mono). The live voice
    // needs no channel: it is summed into the buffer after the mix.
    kiln_audio_init((KilnAudioConfig){
        .sample_rate = SAMPLE_RATE, .latency = 0.16f, .sfx_channels = 1, .music_channels = 0,
    });
    // C5 is 2.38x the baked rate; libdragon asserts above a channel's limit.
    mixer_ch_set_limits(CH_BAKED, 16, SAMPLE_RATE * 2.5f, 0);
    const int baked = kiln_sfx_load("rom:/ksvoice.wav64");

    g_scratch_frames = audio_get_buffer_length();
    g_scratch = malloc(sizeof(int16_t) * 2 * (size_t)g_scratch_frames);
    assertf(g_scratch, "live-voice: no memory for a %d-frame scratch", g_scratch_frames);

    faust_n64_ksvoice_init(SAMPLE_RATE);
    faust_n64_ksvoice_set_gain(LIVE_SET_GAIN);
    g_freq_zone = faust_n64_ksvoice_param("freq");
    g_gate_zone = faust_n64_ksvoice_param("gate");
    float *gain_zone = faust_n64_ksvoice_param("gain");
    if (gain_zone) *gain_zone = LIVE_GAIN_ZONE;
    if (g_gate_zone) *g_gate_zone = 0.0f;

    KilnScene scene;
    kiln_scene_init(&scene);
    kiln_prim_stage(&scene, RGBA32(0x10, 0x16, 0x24, 0xFF), 160.0f, 360.0f);
    scene.fov_deg = 58.0f;
    scene.near_z = 10.0f;
    scene.far_z = 360.0f;

    // ── geometry, built once ──────────────────────────────────────────────
    const uint32_t LIVE_RGB = 0x00F5D4FF, BAKED_RGB = 0xFF9E30FF;
    KilnPrim floor_prim, bead_live, bead_baked, rail_live, rail_baked;
    kiln_prim_floor(&floor_prim, 200.0f, 16, kiln_prim_rgba(0x40, 0x48, 0x60), kiln_prim_rgba(0x34, 0x3C, 0x52));
    kiln_prim_box(&bead_live,  (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 3, 3, 3 }},
                  kiln_prim_shade(LIVE_RGB, 1.2f), LIVE_RGB, kiln_prim_shade(LIVE_RGB, 0.5f));
    kiln_prim_box(&bead_baked, (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 3, 3, 3 }},
                  kiln_prim_shade(BAKED_RGB, 1.2f), BAKED_RGB, kiln_prim_shade(BAKED_RGB, 0.5f));
    kiln_prim_box(&rail_live,  (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 84, 2, 6 }},
                  kiln_prim_shade(LIVE_RGB, 0.45f), kiln_prim_shade(LIVE_RGB, 0.3f), kiln_prim_rgba(0x10, 0x10, 0x18));
    kiln_prim_box(&rail_baked, (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 84, 2, 6 }},
                  kiln_prim_shade(BAKED_RGB, 0.45f), kiln_prim_shade(BAKED_RGB, 0.3f), kiln_prim_rgba(0x10, 0x10, 0x18));

    // One transform per drawn object (the RSP reads matrices asynchronously).
    KilnTransform floor_xf, live_xf[STRING_N], baked_xf[STRING_N], rail_xf[2];
    kiln_transform_init(&floor_xf);
    for (int i = 0; i < STRING_N; i++) { kiln_transform_init(&live_xf[i]); kiln_transform_init(&baked_xf[i]); }
    for (int i = 0; i < 2; i++) kiln_transform_init(&rail_xf[i]);
    rail_xf[0].pos = (fm_vec3_t){{ 0, LIVE_Y - 8.0f, 30 }};
    rail_xf[1].pos = (fm_vec3_t){{ 0, BAKED_Y - 8.0f, 30 }};

    if (KILN_JUMP == JUMP_AB) kiln_input_play(1, &AB_TAPE);
    else kiln_input_set_attract(1, &ATTRACT, 180);

    int sel = 0, repeat = 0;
    float now = 0.0f, baked_end = -1.0f, live_flash = -10.0f, baked_flash = -10.0f;
    int last_live = -1, last_baked = -1;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        const uint32_t ticks = get_ticks();
        now += clampf((float)TICKS_DISTANCE(last_ticks, ticks) / (float)TICKS_PER_SECOND, 0.0f, 0.1f);
        last_ticks = ticks;

        // ── input ───────────────────────────────────────────────────────
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);
        int move = 0;
        if (in->edges & (KILN_BTN_DR | KILN_BTN_CR)) move = 1;
        if (in->edges & (KILN_BTN_DL | KILN_BTN_CL)) move = -1;
        if (in->stick_x > 0.5f || in->stick_x < -0.5f) {
            if (repeat <= 0) { move = in->stick_x > 0 ? 1 : -1; repeat = 14; }
            else repeat--;
        } else {
            repeat = 0;
        }
        sel = (int)clampf((float)(sel + move), 0.0f, NOTES - 1);

        if (in->edges & (KILN_BTN_A | KILN_BTN_Z)) {
            // Queued, not applied: the pump consumes it whenever it next
            // renders, on this frame or a later one.
            g_live_pending_hz = NOTE_HZ[sel];
            if (g_live_pending < 2) g_live_pending++;
            live_flash = now;
            last_live = sel;
        }
        if (in->edges & (KILN_BTN_B | KILN_BTN_Z)) {
            const float ratio = NOTE_HZ[sel] / BAKED_HZ;
            if (kiln_sfx_play(baked, CH_BAKED, 1) >= 0) {
                kiln_sfx_set_pitch(CH_BAKED, ratio);
                // Stop a little before the sample would end (see examples/audio).
                baked_end = now + BAKED_SECS / ratio - 0.1f;
            }
            baked_flash = now;
            last_baked = sel;
        }
        if (baked_end > 0.0f && now >= baked_end) {
            kiln_sfx_stop(CH_BAKED);
            baked_end = -1.0f;
        }

        // ── camera ──────────────────────────────────────────────────────
        scene.cam_pos = (fm_vec3_t){{ fm_sinf(now * 0.25f) * 30.0f, 58.0f, -150.0f }};
        scene.cam_target = (fm_vec3_t){{ 0, 36, 30 }};
        kiln_scene_update(&scene);

        // ── 3D: the last buffer of each voice, as a string of beads ──────
        kiln_frame_begin();
        kiln_scene_begin(&scene);
        kiln_transform_push(&floor_xf); kiln_prim_draw(&floor_prim); kiln_transform_pop();
        kiln_transform_push(&rail_xf[0]); kiln_prim_draw(&rail_live);  kiln_transform_pop();
        kiln_transform_push(&rail_xf[1]); kiln_prim_draw(&rail_baked); kiln_transform_pop();

        for (int i = 0; i < STRING_N; i++) {
            // Screen-right is -X: sample 0 on the left.
            const float x = ((float)i - (STRING_N - 1) * 0.5f) * -7.0f;
            const int si = i * SCOPE_N / STRING_N;
            live_xf[i].pos  = (fm_vec3_t){{ x, LIVE_Y  + clampf(g_scope_live.s[si]  / 600.0f, -12, 12), 30 }};
            baked_xf[i].pos = (fm_vec3_t){{ x, BAKED_Y + clampf(g_scope_baked.s[si] / 600.0f, -12, 12), 30 }};
            kiln_transform_push(&live_xf[i]);  kiln_prim_draw(&bead_live);  kiln_transform_pop();
            kiln_transform_push(&baked_xf[i]); kiln_prim_draw(&bead_baked); kiln_transform_pop();
        }

        // ── 2D ──────────────────────────────────────────────────────────
        kiln_gui_begin();
        const color_t ink   = RGBA32(0xE8, 0xE8, 0xF0, 0xFF);
        const color_t dim   = RGBA32(0x90, 0x98, 0xB0, 0xFF);
        const color_t live  = RGBA32(0x00, 0xF5, 0xD4, 0xFF);
        const color_t bake  = RGBA32(0xFF, 0x9E, 0x30, 0xFF);
        const color_t panel = RGBA32(0x0C, 0x10, 0x1C, 0xFF);

        kiln_gui_panel(8, 8, 178, 48, panel, live);
        kiln_gui_text(14, 21, live, "KILN LIVE VOICE");
        kiln_gui_text(14, 33, ink, "note %s  %5.1f Hz", NOTE_NAME[sel], (double)NOTE_HZ[sel]);
        kiln_gui_text(14, 45, dim, "VR4300 %4.2f ms / %d fr", (double)g_render_ms, g_scratch_frames);

        // Which voice sounded last, and at what pitch.
        const int rx = SCREEN_W - 126;
        kiln_gui_panel(rx, 8, 118, 48, panel, RGBA32(0x8B, 0x5C, 0xF6, 0xFF));
        kiln_gui_rect(rx + 6, 15, 8, 8, now - live_flash < 0.3f ? live : RGBA32(0x1C, 0x40, 0x3C, 0xFF));
        kiln_gui_text(rx + 18, 23, live, "live  %s", last_live < 0 ? "--" : NOTE_NAME[last_live]);
        kiln_gui_rect(rx + 6, 33, 8, 8, now - baked_flash < 0.3f ? bake : RGBA32(0x44, 0x30, 0x14, 0xFF));
        kiln_gui_text(rx + 18, 41, bake, "baked %s", last_baked < 0 ? "--" : NOTE_NAME[last_baked]);
        kiln_gui_text(rx + 18, 52, dim, "%s", kiln_sfx_playing(CH_BAKED) ? "ch0 playing" : "ch0 idle");

        if (kiln_input_scripted(1)) {
            kiln_gui_panel(SCREEN_W - 58, 62, 50, 16, RGBA32(0xC0, 0x30, 0x60, 0xFF), RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
            kiln_gui_text(SCREEN_W - 49, 74, RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "DEMO");
        }

        // Labels on the strings, projected.
        int lx, ly;
        // Left end of each rail (screen-left is +X).
        if (kiln_scene_project(&scene, (fm_vec3_t){{ 84, LIVE_Y + 16.0f, 30 }}, SCREEN_W, SCREEN_H, &lx, &ly))
            kiln_gui_text(lx, ly, live, "CPU");
        if (kiln_scene_project(&scene, (fm_vec3_t){{ 84, BAKED_Y + 16.0f, 30 }}, SCREEN_W, SCREEN_H, &lx, &ly))
            kiln_gui_text(lx, ly, bake, "RSP");

        draw_scope(8, SCREEN_H - 64, 150, &g_scope_live, "live CPU", live);
        draw_scope(162, SCREEN_H - 64, 150, &g_scope_baked, "baked RSP", bake);

        kiln_gui_panel(8, SCREEN_H - 24, SCREEN_W - 16, 16, panel, RGBA32(0x8B, 0x5C, 0xF6, 0xFF));
        kiln_gui_text(14, SCREEN_H - 12, ink, "stick note  A live  B baked  Z both");

        kiln_gui_end();
        kiln_frame_end();

        audio_pump();
    }
}
