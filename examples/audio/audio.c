// SPDX-License-Identifier: MIT
//
// Report Stage 1 played as an instrument: ONE baked pluck, retuned per note.
//
// dsp/ks.dsp is rendered offline at full quality, VADPCM-encoded by
// audioconv64 into rom:/ksvoice.wav64 (a 2 s Karplus-Strong string at A3), and
// every note here is that same file started on a free mixer channel and pitched
// by the RSP: kiln_sfx_play_ex + kiln_sfx_set_pitch. Nothing is synthesised on
// the VR4300 — the expensive thing on this console is synthesis, not playback,
// and this is what "bake 80-90% of the audio" sounds like in practice.
//
//   a sequencer   A minor pentatonic over two octaves, two bars that alternate,
//                 with a root/fifth bass an octave below the lowest key
//   the keys      ten pedestals on an arc; a note lifts its tine and throws a
//                 ring across the floor that fades as the pluck decays
//   the HUD       a scope of what the RSP actually mixed (kiln_audio's output
//                 tap, not a model of it), the last 40 notes as bars, voices
//
//   stick / D-pad  pick a key      A  pluck it      B  pluck a triad
//   Z              sequencer on/off                 idle 3 s: the demo plays
//
// ── Why every note is stopped by hand ───────────────────────────────────
// A pluck pitched up by r lasts 2/r seconds. flake.nix's kilnJingle comment
// records a non-looping wav64 reaching its end asserting inside the mixer
// ("samplebuffer_get: no reader to extend"). The pinned mixer now stops a
// one-shot at its end (mixer.c's mixer_update_loops), but this demo retriggers
// eight channels several times a second, which is exactly where to not lean on
// it: each voice is faded over its last quarter second and stopped before the
// sample would end.
//
// ── Why the channels' limits are raised ─────────────────────────────────
// The top key is 3.56x the encoded rate, 114 kHz against a 32 kHz output, and
// libdragon's mixer asserts on any frequency above a channel's limit — which
// defaults to the output rate. mixer_ch_set_limits raises it for the 8 voices.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_audio.h>
#include <kiln/kiln_prim.h>

#include <string.h>

#define SCREEN_W 320
#define SCREEN_H 240

// Must match the sampleRate given to mkBakedInstrument (mkN64Rom's audioRate
// cross-checks it at build time).
#define SAMPLE_RATE 32000
#define SAMPLE_SECS 2.0f

#define VOICES    8
#define KEYS      10
#define MAX_RATIO 4.0f
#define FADE_SECS 0.25f

#define RINGS     12
#define RING_LIFE 1.4f
#define RING_LEVELS 4
#define HISTORY   40
#define SCOPE_N   96

#define SEQ_STEPS 32
#define STEP_SECS 0.18f

// A minor pentatonic from A3, the pitch ks-baked is rendered at: semitones
// 0 3 5 7 10 12 15 17 19 22, as ratios (2^(s/12), precomputed — no powf).
static const float KEY_RATIO[KEYS] = {
    1.000000f, 1.189207f, 1.334840f, 1.498307f, 1.781797f,
    2.000000f, 2.378414f, 2.669680f, 2.996614f, 3.563595f,
};
static const char *const KEY_NAME[KEYS] = {
    "A3", "C4", "D4", "E4", "G4", "A4", "C5", "D5", "E5", "G5",
};
static const uint32_t KEY_RGB[KEYS] = {
    0x00F5D4FF, 0x38BDF8FF, 0x6D8BFFFF, 0x8B5CF6FF, 0xC04CF0FF,
    0xFF4C9AFF, 0xFF6A4CFF, 0xFF9E30FF, 0xFFD94CFF, 0xB8F04CFF,
};
#define BASS_RGB 0xE8E8F0FF

// Two bars of melody (key index, -1 rest) and a bass line under them (a ratio
// of A3; 0 = none). 0.5 is A2, 0.749 is E3.
static const int8_t SEQ_MELODY[SEQ_STEPS] = {
    0, -1, 2, 3,   4, -1, 3, 2,   5, -1, 4, 3,   2, -1, 1, -1,
    0, -1, 2, 3,   4, 5, 7, -1,   6, -1, 5, 4,   3, 2, 1, -1,
};
static const float SEQ_BASS[SEQ_STEPS] = {
    0.5f, 0, 0, 0,   0, 0, 0, 0,   0.749f, 0, 0, 0,   0, 0, 0, 0,
    0.5f, 0, 0, 0,   0, 0, 0, 0,   0.749f, 0, 0, 0,   0.5f, 0, 0, 0,
};

typedef struct {
    int   active;
    int   key;        // -1 = bass
    float t_end;
    float vol, pan;
} Voice;

typedef struct {
    int       active;
    int       key;    // -1 = bass
    float     t0;
    fm_vec3_t pos;
} Ring;

static Voice  g_voice[VOICES];
static Ring   g_ring[RINGS];
static float  g_hit[KEYS];
static int8_t g_hist[HISTORY];
static int    g_hist_head;
static int    g_sfx = -1;

// ── Output tap: the last mixed buffer, triggered on a rising zero crossing ──
static int16_t g_scope[SCOPE_N];
static int     g_scope_peak;

static void scope_tap(const int16_t *s, int frames, void *ctx)
{
    (void)ctx;
    int start = 0;
    for (int i = 1; i < frames / 2; i++) {
        if (s[(i - 1) * 2] < 0 && s[i * 2] >= 0) { start = i; break; }
    }
    int peak = 0;
    for (int i = 0; i < SCOPE_N; i++) {
        int f = start + i * 3;
        if (f >= frames) f = frames - 1;
        const int v = ((int)s[f * 2] + (int)s[f * 2 + 1]) / 2;
        g_scope[i] = (int16_t)v;
        if (v > peak) peak = v;
        if (-v > peak) peak = -v;
    }
    g_scope_peak = peak;
}

// ── Attract tape: walk the cursor up and back, plucking, over the sequence ──
static const KilnInputKey ATTRACT_KEYS[] = {
    { .frame =   0 },
    { .frame =  30, .sx =  70 },
    { .frame =  44 },
    { .frame =  60, .buttons = KILN_BTN_A },
    { .frame =  66 },
    { .frame =  90, .buttons = KILN_BTN_A },
    { .frame =  96 },
    { .frame = 120, .sx =  70 },
    { .frame = 150 },
    { .frame = 160, .buttons = KILN_BTN_B },
    { .frame = 166 },
    { .frame = 200, .sx =  70 },
    { .frame = 214 },
    { .frame = 230, .buttons = KILN_BTN_A },
    { .frame = 236 },
    { .frame = 270, .sx = -70 },
    { .frame = 310 },
    { .frame = 320, .buttons = KILN_BTN_B },
    { .frame = 326 },
    { .frame = 350, .buttons = KILN_BTN_A },
    { .frame = 356 },
    { .frame = 400 },
};
static const KilnInputTape ATTRACT = { ATTRACT_KEYS, sizeof ATTRACT_KEYS / sizeof ATTRACT_KEYS[0], 0 };

// ── A ring: 16 flat quads facing +Y, radius 28..32 at scale 1 ───────────────
static void ring_vert(T3DVertPacked *base, int vi, float x, float z, uint32_t rgba, uint16_t norm)
{
    T3DVertPacked *p = &base[vi / 2];
    const int16_t pos[3] = { (int16_t)(x < 0 ? x - 0.5f : x + 0.5f), 0,
                             (int16_t)(z < 0 ? z - 0.5f : z + 0.5f) };
    if (vi & 1) { memcpy(p->posB, pos, sizeof pos); p->normB = norm; p->rgbaB = rgba; }
    else        { memcpy(p->posA, pos, sizeof pos); p->normA = norm; p->rgbaA = rgba; }
}

static int ring_build(KilnPrim *out, uint32_t rgba)
{
    enum { SEG = 16 };
    const float RIN = 28.0f, ROUT = 32.0f;
    memset(out, 0, sizeof *out);
    out->verts = malloc_uncached(sizeof(T3DVertPacked) * SEG * 2);
    if (!out->verts) return -1;
    memset(out->verts, 0, sizeof(T3DVertPacked) * SEG * 2);
    out->quad_count = SEG;
    out->vert_count = SEG * 4;
    fm_vec3_t up = {{ 0, 1, 0 }};
    const uint16_t norm = t3d_vert_pack_normal(&up);
    for (int s = 0; s < SEG; s++) {
        const float a0 = (float)s * 6.2831853f / SEG, a1 = (float)(s + 1) * 6.2831853f / SEG;
        const float c0 = fm_cosf(a0), s0 = fm_sinf(a0), c1 = fm_cosf(a1), s1 = fm_sinf(a1);
        // inner0, inner1, outer1, outer0: counter-clockwise seen from +Y.
        ring_vert(out->verts, s * 4 + 0, RIN * c0,  RIN * s0,  rgba, norm);
        ring_vert(out->verts, s * 4 + 1, RIN * c1,  RIN * s1,  rgba, norm);
        ring_vert(out->verts, s * 4 + 2, ROUT * c1, ROUT * s1, rgba, norm);
        ring_vert(out->verts, s * 4 + 3, ROUT * c0, ROUT * s0, rgba, norm);
    }
    data_cache_hit_writeback(out->verts, sizeof(T3DVertPacked) * SEG * 2);
    return 0;
}

// ── Keys on an arc; screen-right is -X with the camera looking down +Z ─────
static fm_vec3_t key_pos(int k)
{
    const float a = (-55.0f + 110.0f * (float)k / (KEYS - 1)) * 0.0174533f;
    return (fm_vec3_t){{ -fm_sinf(a) * 110.0f, 0.0f, fm_cosf(a) * 110.0f - 40.0f }};
}

// 1 at the hit, easing to 0 over 0.9 s.
static float key_env(float age)
{
    if (age < 0.0f || age > 0.9f) return 0.0f;
    const float e = 1.0f - age / 0.9f;
    return e * e;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void pluck(int key, float ratio, float vol, int pri, float now)
{
    const float pan = key < 0 ? 0.5f : 0.2f + 0.6f * (float)key / (KEYS - 1);
    const int ch = kiln_sfx_play_ex(g_sfx, -1, pri, vol, pan);
    if (ch < 0 || ch >= VOICES) return;
    kiln_sfx_set_pitch(ch, ratio);
    g_voice[ch] = (Voice){ .active = 1, .key = key, .vol = vol, .pan = pan,
                           .t_end = now + SAMPLE_SECS / ratio - 0.12f };

    if (key >= 0) g_hit[key] = now;
    g_hist[g_hist_head] = (int8_t)key;
    g_hist_head = (g_hist_head + 1) % HISTORY;

    // Reuse the oldest ring when all are busy.
    int slot = 0;
    for (int i = 0; i < RINGS; i++) {
        if (!g_ring[i].active) { slot = i; break; }
        if (g_ring[i].t0 < g_ring[slot].t0) slot = i;
    }
    g_ring[slot] = (Ring){ .active = 1, .key = key, .t0 = now,
                           .pos = key < 0 ? (fm_vec3_t){{ 0, 0, 30 }} : key_pos(key) };
}

static void voices_update(float now)
{
    for (int ch = 0; ch < VOICES; ch++) {
        Voice *v = &g_voice[ch];
        if (!v->active) continue;
        if (!kiln_sfx_playing(ch)) { v->active = 0; continue; }
        const float left = v->t_end - now;
        if (left <= 0.0f) {
            kiln_sfx_stop(ch);
            v->active = 0;
        } else if (left < FADE_SECS) {
            kiln_sfx_set_vol_pan(ch, v->vol * left / FADE_SECS, v->pan);
        }
    }
}

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    // Mount the ROM's DragonFS before anything opens rom:/.
    dfs_init(DFS_DEFAULT_LOCATION);
    joypad_init();
    kiln_input_init();

    kiln_audio_init((KilnAudioConfig){
        .sample_rate = SAMPLE_RATE, .latency = 0.16f, .sfx_channels = VOICES, .music_channels = 0,
    });
    for (int ch = 0; ch < VOICES; ch++)
        mixer_ch_set_limits(ch, 16, SAMPLE_RATE * MAX_RATIO, 0);
    g_sfx = kiln_sfx_load("rom:/ksvoice.wav64");
    kiln_audio_set_tap(scope_tap, NULL);

    KilnScene scene;
    kiln_scene_init(&scene);
    kiln_prim_stage(&scene, RGBA32(0x14, 0x18, 0x2E, 0xFF), 200.0f, 420.0f);
    scene.fov_deg = 60.0f;
    scene.near_z = 10.0f;
    scene.far_z = 420.0f;

    // ── geometry, built once: nothing below is rewritten after this ────────
    KilnPrim floor_prim, pedestal[KEYS], tine_lit[KEYS], tine_dim[KEYS], cursor;
    KilnPrim ring[KEYS + 1][RING_LEVELS];
    static const float LEVEL_SHADE[RING_LEVELS] = { 1.0f, 0.7f, 0.45f, 0.25f };
    kiln_prim_floor(&floor_prim, 220.0f, 16, kiln_prim_rgba(0x46, 0x4C, 0x6A), kiln_prim_rgba(0x3A, 0x40, 0x5C));
    for (int k = 0; k < KEYS; k++) {
        const uint32_t c = KEY_RGB[k];
        kiln_prim_box(&pedestal[k], (fm_vec3_t){{ 0, 4, 0 }}, (fm_vec3_t){{ 9, 4, 9 }},
                      kiln_prim_shade(c, 0.55f), kiln_prim_rgba(0x5A, 0x60, 0x80), kiln_prim_rgba(0x20, 0x22, 0x30));
        kiln_prim_box(&tine_lit[k], (fm_vec3_t){{ 0, 10, 0 }}, (fm_vec3_t){{ 4, 10, 4 }},
                      kiln_prim_shade(c, 1.25f), c, c);
        kiln_prim_box(&tine_dim[k], (fm_vec3_t){{ 0, 10, 0 }}, (fm_vec3_t){{ 4, 10, 4 }},
                      kiln_prim_shade(c, 0.6f), kiln_prim_shade(c, 0.4f), kiln_prim_shade(c, 0.4f));
    }
    for (int k = 0; k <= KEYS; k++)
        for (int l = 0; l < RING_LEVELS; l++)
            ring_build(&ring[k][l], kiln_prim_shade(k < KEYS ? KEY_RGB[k] : BASS_RGB, LEVEL_SHADE[l]));
    kiln_prim_box(&cursor, (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 3, 3, 3 }},
                  kiln_prim_rgba(0xFF, 0xFF, 0xFF), kiln_prim_rgba(0xFF, 0xE0, 0x60), kiln_prim_rgba(0x80, 0x60, 0x20));

    // One transform per drawn object: the RSP reads matrices asynchronously,
    // so a transform reused between two draws in a frame would race it.
    KilnTransform floor_xf, ped_xf[KEYS], tine_xf[KEYS], ring_xf[RINGS], cursor_xf;
    kiln_transform_init(&floor_xf);
    kiln_transform_init(&cursor_xf);
    for (int k = 0; k < KEYS; k++) {
        kiln_transform_init(&ped_xf[k]);
        kiln_transform_init(&tine_xf[k]);
        ped_xf[k].pos = key_pos(k);
        tine_xf[k].pos = key_pos(k);
        tine_xf[k].pos.v[1] = 8.0f;
        g_hit[k] = -10.0f;
    }
    for (int i = 0; i < RINGS; i++) kiln_transform_init(&ring_xf[i]);
    memset(g_hist, -2, sizeof g_hist);   // -2 = empty slot

    kiln_input_set_attract(1, &ATTRACT, 180);

    float now = 0.0f, seq_next = 0.5f;
    int step = 0, seq_on = 1, sel = 4, repeat = 0;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        const uint32_t ticks = get_ticks();
        const float dt = clampf((float)TICKS_DISTANCE(last_ticks, ticks) / (float)TICKS_PER_SECOND, 0.0f, 0.1f);
        last_ticks = ticks;
        now += dt;

        // ── input ───────────────────────────────────────────────────────
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);
        int move = 0;
        if (in->edges & KILN_BTN_DR) move = 1;
        if (in->edges & KILN_BTN_DL) move = -1;
        if (in->stick_x > 0.5f || in->stick_x < -0.5f) {
            if (repeat <= 0) { move = in->stick_x > 0 ? 1 : -1; repeat = repeat < 0 ? 6 : 14; }
            else repeat--;
        } else {
            repeat = 0;
        }
        sel = (int)clampf((float)(sel + move), 0.0f, KEYS - 1);
        if (in->edges & KILN_BTN_A) pluck(sel, KEY_RATIO[sel], 0.9f, 3, now);
        if (in->edges & KILN_BTN_B) {
            // Root, third and fifth of the pentatonic from the selected key,
            // shifted down at the top of the arc so all three exist.
            const int root = sel < KEYS - 4 ? sel : KEYS - 5;
            for (int j = 0; j < 3; j++)
                pluck(root + j * 2, KEY_RATIO[root + j * 2], 0.6f, 3, now);
        }
        if (in->edges & KILN_BTN_Z) seq_on = !seq_on;

        // ── sequencer ───────────────────────────────────────────────────
        if (now - seq_next > 0.5f) seq_next = now;   // after a stall, don't flam
        while (now >= seq_next) {
            if (seq_on) {
                if (SEQ_BASS[step] > 0.0f) pluck(-1, SEQ_BASS[step], 0.8f, 4, now);
                if (SEQ_MELODY[step] >= 0) {
                    const int k = SEQ_MELODY[step];
                    pluck(k, KEY_RATIO[k], 0.55f, 2, now);
                }
            }
            step = (step + 1) % SEQ_STEPS;
            seq_next += STEP_SECS;
        }
        voices_update(now);

        // ── camera: a slow sway over the arc ────────────────────────────
        scene.cam_pos = (fm_vec3_t){{ fm_sinf(now * 0.2f) * 45.0f, 95.0f + fm_sinf(now * 0.13f) * 10.0f, -150.0f }};
        scene.cam_target = (fm_vec3_t){{ 0, 12, 40 }};
        kiln_scene_update(&scene);

        // ── 3D ──────────────────────────────────────────────────────────
        kiln_frame_begin();
        kiln_scene_begin(&scene);

        kiln_transform_push(&floor_xf); kiln_prim_draw(&floor_prim); kiln_transform_pop();

        for (int i = 0; i < RINGS; i++) {
            Ring *r = &g_ring[i];
            if (!r->active) continue;
            const float age = now - r->t0;
            if (age >= RING_LIFE) { r->active = 0; continue; }
            const int level = (int)(age / RING_LIFE * RING_LEVELS);
            const float grow = (r->key < 0 ? 1.6f : 1.0f) * (0.3f + 1.9f * age / RING_LIFE);
            ring_xf[i].pos = r->pos;
            ring_xf[i].pos.v[1] = 0.8f;
            ring_xf[i].scale = (fm_vec3_t){{ grow, 1.0f, grow }};
            kiln_transform_push(&ring_xf[i]);
            kiln_prim_draw(&ring[r->key < 0 ? KEYS : r->key][level]);
            kiln_transform_pop();
        }

        for (int k = 0; k < KEYS; k++) {
            const float env = key_env(now - g_hit[k]);
            kiln_transform_push(&ped_xf[k]); kiln_prim_draw(&pedestal[k]); kiln_transform_pop();
            tine_xf[k].scale = (fm_vec3_t){{ 1.0f, 0.4f + 2.6f * env, 1.0f }};
            kiln_transform_push(&tine_xf[k]);
            kiln_prim_draw(env > 0.05f ? &tine_lit[k] : &tine_dim[k]);
            kiln_transform_pop();
        }

        const float sel_h = 8.0f + 20.0f * (0.4f + 2.6f * key_env(now - g_hit[sel]));
        cursor_xf.pos = key_pos(sel);
        cursor_xf.pos.v[1] = sel_h + 10.0f + 3.0f * fm_sinf(now * 4.0f);
        cursor_xf.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
        cursor_xf.rot_angle = now * 2.0f;
        kiln_transform_push(&cursor_xf); kiln_prim_draw(&cursor); kiln_transform_pop();

        // ── 2D ──────────────────────────────────────────────────────────
        kiln_gui_begin();
        const color_t ink  = RGBA32(0xE8, 0xE8, 0xF0, 0xFF);
        const color_t dim  = RGBA32(0x90, 0x98, 0xB0, 0xFF);
        const color_t teal = RGBA32(0x00, 0xF5, 0xD4, 0xFF);
        const color_t panel = RGBA32(0x0C, 0x10, 0x1C, 0xFF);

        int voices = 0;
        for (int ch = 0; ch < VOICES; ch++) voices += g_voice[ch].active;

        kiln_gui_panel(8, 8, 150, 48, panel, teal);
        kiln_gui_text(14, 21, teal, "KILN AUDIO");
        kiln_gui_text(14, 33, ink, "ksvoice.wav64 VADPCM");
        kiln_gui_text(14, 45, ink, "voices %d/%d  seq %s", voices, VOICES, seq_on ? "on" : "off");
        kiln_gui_bar(118, 17, 34, 5, (float)step / SEQ_STEPS, teal, RGBA32(0x2A, 0x2A, 0x3E, 0xFF));

        // What the RSP mixed last buffer, not a picture of what it should have.
        const int sx0 = SCREEN_W - 120, sy0 = 8;
        kiln_gui_panel(sx0, sy0, 112, 48, panel, RGBA32(0x8B, 0x5C, 0xF6, 0xFF));
        kiln_gui_text(sx0 + 6, sy0 + 13, dim, "RSP mix");
        kiln_gui_bar(sx0 + 56, sy0 + 6, 50, 5, g_scope_peak / 32768.0f,
                     RGBA32(0xFF, 0xD9, 0x4C, 0xFF), RGBA32(0x2A, 0x2A, 0x3E, 0xFF));
        const int mid = sy0 + 32;
        for (int i = 1; i < SCOPE_N; i++) {
            const int x0 = sx0 + 8 + (i - 1) * 96 / SCOPE_N, x1 = sx0 + 8 + i * 96 / SCOPE_N;
            const int y0 = mid - (int)clampf(g_scope[i - 1] / 600.0f, -12, 12);
            const int y1 = mid - (int)clampf(g_scope[i] / 600.0f, -12, 12);
            kiln_gui_line(x0, y0, x1, y1, 1, teal);
        }

        if (kiln_input_scripted(1)) {
            kiln_gui_panel(SCREEN_W - 58, 62, 50, 16, RGBA32(0xC0, 0x30, 0x60, 0xFF), RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
            kiln_gui_text(SCREEN_W - 49, 74, RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "DEMO");
        }

        // The selected key's name, over the key.
        int lx, ly;
        fm_vec3_t top = cursor_xf.pos;
        top.v[1] += 12.0f;
        if (kiln_scene_project(&scene, top, SCREEN_W, SCREEN_H, &lx, &ly))
            kiln_gui_text(lx - 6, ly, RGBA32(0xFF, 0xE0, 0x60, 0xFF), "%s", KEY_NAME[sel]);

        // Note history: oldest at the left, bass as a short white bar.
        const int hy = SCREEN_H - 64;
        kiln_gui_panel(8, hy, SCREEN_W - 16, 38, panel, RGBA32(0x3A, 0x40, 0x5C, 0xFF));
        for (int i = 0; i < HISTORY; i++) {
            const int k = g_hist[(g_hist_head + i) % HISTORY];
            if (k == -2) continue;
            const int h = k < 0 ? 5 : 8 + k * 2;
            const uint32_t rgb = k < 0 ? BASS_RGB : KEY_RGB[k];
            kiln_gui_rect(14 + i * 7, hy + 33 - h, 5, h,
                          RGBA32(rgb >> 24, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, 0xFF));
        }

        kiln_gui_panel(8, SCREEN_H - 24, SCREEN_W - 16, 16, panel, RGBA32(0x8B, 0x5C, 0xF6, 0xFF));
        kiln_gui_text(14, SCREEN_H - 12, ink, "stick key  A pluck  B triad  Z seq");

        kiln_gui_end();
        kiln_frame_end();

        kiln_audio_update();
    }
}
