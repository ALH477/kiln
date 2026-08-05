// SPDX-License-Identifier: MPL-2.0
//
// Bass-synth — a 4-controller collaborative bass ROM.
//
// Architecture (single file, four sections):
//
//   1. Wavetables — 16 single-cycle 256-sample mono wavs, baked in flake.nix
//      as looping VADPCM wav64s. 4 engines x 4 variants each. The RSP mixer
//      pitches them live: to play a tone at f_hz, set mixer freq = f_hz * 256.
//
//   2. Voice allocator — 12-voice pool with low-note-priority stealing.
//      Each voice owns a fixed mixer channel (0..11). Note-on finds a free
//      voice or steals the highest pitched one. AR envelope ramps vol per
//      frame; release completes by stopping the channel.
//
//   3. Controller mapping — 13 buttons per pad (A/B/Z/L/R + C-pad + D-pad)
//      mapped to a 13-note chromatic scale across the player's octave.
//      Each player has an octave offset (P1 lowest, P4 highest) and a
//      per-player engine. Stick X/Y pick the wavetable variant at note-on
//      (bright/dark/clean/driven); stick magnitude applies a small detune
//      per frame. Z = sustain (skip release on note-off until Z released).
//
//   4. UI — Start toggles a 2D menu (per-player engine + octave). When the
//      menu is open, gameplay notes are suppressed. An always-on 2D overlay
//      shows per-player status + stick viz + active-voice bars. Behind the
//      UI, a 3D "spectrum bar" field draws 12 thin cubes Y-scaled per voice,
//      colour-tinted by engine — the only 3D in the ROM.
//
// All VR4300 work is per-frame (envelope + param updates for <=12 voices);
// the RSP mixer does the actual sample mixing. No Faust, no per-sample DSP.

#include <libdragon.h>
#include <math.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>

#include <m64/m64_engine.h>
#include <m64/m64_gui.h>
#include <m64/m64_input.h>
#include <m64/m64_audio.h>

#define SCREEN_W    320
#define SCREEN_H    240
#define SAMPLE_RATE 32000

#define BASS_VOICES  12
#define WT_LEN       256        // samples per single-cycle wavetable

// Lowest playable MIDI note (E2 = 28 on a 4-string bass).
#define BASE_MIDI    28

// Envelope timings.
#define ATTACK_MS    5.0f
#define RELEASE_MS   80.0f

// ── Engines ──────────────────────────────────────────────────────────────
typedef enum {
    BASS_HEAVY = 0,
    BASS_SUB,
    BASS_GROWL,
    BASS_INDUSTRIAL,
    BASS_ENGINE_COUNT
} BassEngine;

// Per-engine variant: which of the 4 wavetables to use, picked by stick.
typedef enum {
    BASS_BRIGHT = 0,
    BASS_DARK,
    BASS_CLEAN,
    BASS_DRIVEN,
    BASS_VARIANT_COUNT
} BassVariant;

static const char * const ENGINE_NAME[BASS_ENGINE_COUNT] = {
    "HEAVY", "SUB", "GROWL", "INDUST",
};
static const char * const VARIANT_NAME[BASS_VARIANT_COUNT] = {
    "BRIGHT", "DARK", "CLEAN", "DRIVEN",
};

// g_wt[engine][variant] → m64_sfx_load handle.
static int g_wt[BASS_ENGINE_COUNT][BASS_VARIANT_COUNT];

// ── Voices ───────────────────────────────────────────────────────────────
typedef struct {
    uint8_t  active;
    uint8_t  channel;     // fixed: == index in g_voices
    uint8_t  player;      // 1..4
    uint8_t  engine;
    uint8_t  variant;
    int8_t   note;        // MIDI note
    float    freq_hz;     // base tone frequency
    float    age_ms;
    float    vol_current;
    float    vol_target;
    uint8_t  releasing;
    uint8_t  sustained;   // Z held at note-off — don't release until Z released
} BassVoice;

static BassVoice g_voices[BASS_VOICES];

// ── Players ──────────────────────────────────────────────────────────────
typedef struct {
    int8_t   engine;       // BassEngine
    int8_t   octave;       // octave offset in semitones (12 = +1 octave)
    float    stick_x;
    float    stick_y;
    float    stick_mag;
    uint8_t  z_held;
    // Per-player last note (for the activity overlay).
    int8_t   last_note;
    uint8_t  last_variant;
} BassPlayer;

#define NPLAYERS 4
static BassPlayer g_players[NPLAYERS] = {
    { .engine = BASS_HEAVY,      .octave = 0  },
    { .engine = BASS_SUB,        .octave = 12 },
    { .engine = BASS_GROWL,      .octave = 24 },
    { .engine = BASS_INDUSTRIAL, .octave = 36 },
};

// Stereo pan per player (P1 left → P4 right).
static const float PLAYER_PAN[NPLAYERS] = { 0.30f, 0.43f, 0.57f, 0.70f };

// Player colours (also used for engine tinting in the cube field).
static const color_t PLAYER_COLOR[NPLAYERS] = {
    RGBA32(0x00, 0xE5, 0xFF, 0xFF),
    RGBA32(0x4C, 0xFF, 0x82, 0xFF),
    RGBA32(0xFF, 0xD9, 0x4C, 0xFF),
    RGBA32(0xFF, 0x4C, 0x6A, 0xFF),
};

// ── Note mapping ──────────────────────────────────────────────────────────
// 13 buttons in low-to-high pitch order, each a semitone offset within the
// player's octave (0..12 = one octave + 1 note).
static const uint32_t NOTE_BUTTONS[13] = {
    M64_BTN_DU,  // 0
    M64_BTN_DL,  // 1
    M64_BTN_DD,  // 2
    M64_BTN_DR,  // 3
    M64_BTN_CL,  // 4
    M64_BTN_CD,  // 5
    M64_BTN_CR,  // 6
    M64_BTN_CU,  // 7
    M64_BTN_L,   // 8
    M64_BTN_B,   // 9
    M64_BTN_A,   // 10
    M64_BTN_Z,   // 11
    M64_BTN_R,   // 12
};

// ── Menu state ────────────────────────────────────────────────────────────
static uint8_t g_menu_open = 0;
static int8_t  g_menu_row  = 0;        // 0..7: P1engine,P1oct,P2engine,P2oct,...
#define MENU_ROWS (NPLAYERS * 2)

// ── Scene / 3D ────────────────────────────────────────────────────────────
static M64Scene g_scene;

// Per-voice cube transform. 12 cubes laid out along X.
static M64Transform g_cube_xform[BASS_VOICES];
static T3DVertPacked *g_cube_verts;

static const uint8_t CUBE_TRIS[12][3] = {
    {0,1,2},{2,3,0}, {4,6,5},{6,4,7},
    {0,4,5},{5,1,0}, {1,5,6},{6,2,1},
    {2,6,7},{7,3,2}, {3,7,4},{4,0,3},
};

// ── Helpers ───────────────────────────────────────────────────────────────
static inline float midi_to_hz(int midi)
{
    return 440.0f * powf(2.0f, (midi - 69) / 12.0f);
}

// Pick a wavetable variant from stick position. Stick at rest → CLEAN.
// |X| > |Y| → BRIGHT/DARK on X sign. Else → DRIVEN/CLEAN on Y sign.
static BassVariant pick_variant(float sx, float sy)
{
    float ax = sx < 0 ? -sx : sx;
    float ay = sy < 0 ? -sy : sy;
    if (ax < 0.15f && ay < 0.15f) return BASS_CLEAN;
    if (ax >= ay) return sx >= 0 ? BASS_BRIGHT : BASS_DARK;
    return sy > 0 ? BASS_DRIVEN : BASS_CLEAN;
}

// ── Voice allocator ───────────────────────────────────────────────────────
static int voice_find_free(void)
{
    for (int i = 0; i < BASS_VOICES; i++)
        if (!g_voices[i].active) return i;
    return -1;
}

static int voice_find_victim(void)
{
    // Steal the highest-pitched voice (lowest notes are protected).
    // Ties broken by oldest age.
    int victim = -1;
    int best_note = -128;
    float best_age = -1.0f;
    for (int i = 0; i < BASS_VOICES; i++) {
        if (!g_voices[i].active) continue;
        if (g_voices[i].note > best_note ||
            (g_voices[i].note == best_note && g_voices[i].age_ms > best_age)) {
            best_note = g_voices[i].note;
            best_age  = g_voices[i].age_ms;
            victim = i;
        }
    }
    return victim;
}

static void note_on(int player, int8_t midi, int engine, BassVariant variant)
{
    int slot = voice_find_free();
    if (slot < 0) slot = voice_find_victim();
    if (slot < 0) return;  // shouldn't happen — pool not full means free exists

    BassVoice *v = &g_voices[slot];
    int wt_handle = g_wt[engine][variant];
    if (wt_handle < 0) return;

    // Stop any current playback on this channel (wav64_play would replace
    // it anyway, but this clears the priority + resets the envelope state).
    m64_sfx_stop(v->channel);

    // Trigger the wavetable on the voice's fixed channel.
    m64_sfx_play_ex(wt_handle, v->channel, /*priority*/ 1, /*vol*/ 0.0f,
                    PLAYER_PAN[player - 1]);

    float f_hz = midi_to_hz(midi);
    m64_sfx_set_freq(v->channel, f_hz * WT_LEN);

    v->active      = 1;
    v->player       = (uint8_t)player;
    v->engine       = (uint8_t)engine;
    v->variant     = (uint8_t)variant;
    v->note        = midi;
    v->freq_hz     = f_hz;
    v->age_ms      = 0.0f;
    v->vol_current = 0.0f;
    v->vol_target  = 1.0f;
    v->releasing   = 0;
    v->sustained   = 0;
}

static void note_off(int player, int8_t midi)
{
    // Release the most recent voice on this player matching the note.
    // (A player can hold the same button retriggered; release the latest.)
    int found = -1;
    float newest = -1.0f;
    for (int i = 0; i < BASS_VOICES; i++) {
        if (!g_voices[i].active || g_voices[i].player != (uint8_t)player) continue;
        if (g_voices[i].note != midi) continue;
        if (g_voices[i].age_ms > newest) { newest = g_voices[i].age_ms; found = i; }
    }
    if (found < 0) return;

    BassVoice *v = &g_voices[found];
    if (g_players[player - 1].z_held) {
        v->sustained = 1;  // hold until Z is released
    } else {
        v->releasing = 1;
        v->vol_target = 0.0f;
    }
}

static void release_sustained(int player)
{
    for (int i = 0; i < BASS_VOICES; i++) {
        if (!g_voices[i].active || g_voices[i].player != (uint8_t)player) continue;
        if (g_voices[i].sustained) {
            g_voices[i].sustained = 0;
            g_voices[i].releasing = 1;
            g_voices[i].vol_target = 0.0f;
        }
    }
}

// ── Per-frame voice update ────────────────────────────────────────────────
static void update_voices(float dt_ms)
{
    for (int i = 0; i < BASS_VOICES; i++) {
        BassVoice *v = &g_voices[i];
        if (!v->active) continue;

        v->age_ms += dt_ms;

        if (v->releasing) {
            v->vol_current -= dt_ms / RELEASE_MS;
            if (v->vol_current <= 0.0f) {
                m64_sfx_stop(v->channel);
                v->active = 0;
                v->vol_current = 0.0f;
                continue;
            }
        } else {
            // Linear attack toward vol_target.
            float step = dt_ms / ATTACK_MS;
            v->vol_current += step;
            if (v->vol_current > v->vol_target) v->vol_current = v->vol_target;
        }

        // Stick magnitude → small detune, applied per frame so vibrato is
        // playable by wiggling the stick. ±2% of base freq.
        BassPlayer *p = &g_players[v->player - 1];
        float detune = 1.0f + (p->stick_mag - 0.5f) * 0.04f;
        m64_sfx_set_freq(v->channel, v->freq_hz * WT_LEN * detune);
        m64_sfx_set_vol_pan(v->channel, v->vol_current, PLAYER_PAN[v->player - 1]);
    }
}

// ── Input handling ────────────────────────────────────────────────────────
static void handle_play_input(void)
{
    for (int p = 1; p <= NPLAYERS; p++) {
        const M64Input *in = m64_input_get(p);
        if (!in) continue;

        BassPlayer *player = &g_players[p - 1];
        player->stick_x = in->stick_x;
        player->stick_y = in->stick_y;
        player->stick_mag = sqrtf(in->stick_x * in->stick_x +
                                 in->stick_y * in->stick_y);
        if (player->stick_mag > 1.0f) player->stick_mag = 1.0f;
        uint8_t z_now = (in->buttons & M64_BTN_Z) ? 1 : 0;
        uint8_t z_released = z_now == 0 && player->z_held == 1;
        player->z_held = z_now;
        if (z_released) release_sustained(p);

        // Note-on / note-off per button edge.
        BassVariant variant = pick_variant(in->stick_x, in->stick_y);
        player->last_variant = (uint8_t)variant;

        for (int n = 0; n < 13; n++) {
            uint32_t mask = NOTE_BUTTONS[n];
            int8_t midi = BASE_MIDI + player->octave + n;
            if (in->edges & mask) {
                note_on(p, midi, player->engine, variant);
                player->last_note = midi;
            } else if (in->released & mask) {
                // Z is also button index 11 (note offset 11); we use it both
                // as a note AND as the sustain modifier. When Z is the
                // sustain source we still trigger its note — the sustain
                // logic just holds releases *of other notes* until Z is
                // released, which feels natural.
                note_off(p, midi);
            }
        }
    }
}

static void handle_menu_input(void)
{
    const M64Input *in = m64_input_get(1);
    if (!in) return;

    if (in->edges & M64_BTN_DU) g_menu_row--;
    if (in->edges & M64_BTN_DD) g_menu_row++;
    if (g_menu_row < 0) g_menu_row = MENU_ROWS - 1;
    if (g_menu_row >= MENU_ROWS) g_menu_row = 0;

    int player = g_menu_row / 2;
    int field  = g_menu_row & 1;  // 0 = engine, 1 = octave

    if (in->edges & (M64_BTN_DL | M64_BTN_DR)) {
        int dir = (in->edges & M64_BTN_DR) ? +1 : -1;
        if (field == 0) {
            g_players[player].engine = (g_players[player].engine + dir +
                                        BASS_ENGINE_COUNT) % BASS_ENGINE_COUNT;
        } else {
            int8_t o = g_players[player].octave + dir * 12;
            if (o < -12) o = -12;
            if (o > 60)  o = 60;
            g_players[player].octave = o;
        }
    }

    if (in->edges & M64_BTN_START) g_menu_open = 0;
}

// ── 2D UI ─────────────────────────────────────────────────────────────────
static color_t col_player(int p) { return PLAYER_COLOR[p - 1]; }

static const char *note_name(int midi)
{
    static const char *names[12] = {
        "C ","C#","D ","D#","E ","F ","F#","G ","G#","A ","A#","B "
    };
    static char buf[8];
    int oct = midi / 12 - 1;  // MIDI 60 = C5
    snprintf(buf, sizeof(buf), "%s%d", names[midi % 12], oct);
    return buf;
}

// A vertical bar (m64_gui_bar is horizontal only — swap w/h, fill bottom-up).
static void bass_vbar(int x, int y, int w, int h, float frac, color_t fg, color_t bg)
{
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    m64_gui_rect(x, y, w, h, bg);
    int fh = (int)(frac * (h - 2));
    m64_gui_rect(x + 1, y + h - 1 - fh, w - 2, fh, fg);
}

// Stick crosshair inside a bordered box.
static void bass_stick_viz(int x, int y, int w, int h,
                           float sx, float sy, color_t border, color_t dot)
{
    m64_gui_panel(x, y, w, h, RGBA32(10, 10, 24, 200), border);
    int cx = x + w / 2;
    int cy = y + h / 2;
    int px = cx + (int)(sx * (w / 2 - 4));
    int py = cy - (int)(sy * (h / 2 - 4));
    m64_gui_rect(px - 2, py - 2, 4, 4, dot);
}

static void draw_menu(void)
{
    int x = 20, y = 30;
    int row_h = 22;
    int w = 280;

    m64_gui_panel(x - 8, y - 14, w + 16, MENU_ROWS * row_h + 38,
                  RGBA32(10, 10, 24, 220), RGBA32(0, 245, 212, 255));
    m64_gui_text(x, y - 10, RGBA32(0, 245, 212, 255),
                 "BASS SYNTH — per-player setup");
    m64_gui_text(x + 200, y - 10, RGBA32(180, 180, 200, 255),
                 "Start: close");

    for (int p = 0; p < NPLAYERS; p++) {
        for (int f = 0; f < 2; f++) {
            int row = p * 2 + f;
            int ry = y + 8 + row * row_h;
            int selected = (row == g_menu_row);
            color_t row_col = selected
                ? RGBA32(40, 60, 80, 255)
                : RGBA32(20, 20, 36, 200);
            color_t row_brd = selected
                ? RGBA32(0, 245, 212, 255)
                : RGBA32(60, 60, 90, 255);
            m64_gui_panel(x, ry, w, row_h - 4, row_col, row_brd);

            color_t pc = col_player(p + 1);
            if (f == 0) {
                m64_gui_text(x + 6, ry + 4, pc, "P%d engine:", p + 1);
                m64_gui_text(x + 110, ry + 4, RGBA32(232, 232, 240, 255),
                             "%s", ENGINE_NAME[g_players[p].engine]);
            } else {
                m64_gui_text(x + 6, ry + 4, pc, "P%d octave:", p + 1);
                int8_t o = g_players[p].octave;
                m64_gui_text(x + 110, ry + 4, RGBA32(232, 232, 240, 255),
                             "%+d st", (int)o);
            }
        }
    }

    m64_gui_text(x, y + 8 + MENU_ROWS * row_h + 4,
                 RGBA32(180, 180, 200, 255),
                 "D-pad: navigate   L/R: change   Start: resume");
}

static void draw_activity_overlay(void)
{
    int col_w = 76;
    int margin = 4;
    int y = SCREEN_H - 76;

    // Top strip — title + voice count.
    m64_gui_panel(margin, margin, SCREEN_W - 2 * margin, 22,
                  RGBA32(10, 10, 24, 220), RGBA32(139, 92, 246, 255));
    int active = 0;
    for (int i = 0; i < BASS_VOICES; i++) if (g_voices[i].active) active++;
    m64_gui_text(margin + 6, margin + 5, RGBA32(232, 232, 240, 255),
                 "BASS SYNTH  voices %d/%d  %s",
                 active, BASS_VOICES,
                 g_menu_open ? "[MENU]" : "Start: menu");

    // Per-player columns across the bottom.
    int total_w = NPLAYERS * col_w + (NPLAYERS - 1) * margin;
    int x0 = (SCREEN_W - total_w) / 2;

    for (int p = 0; p < NPLAYERS; p++) {
        int x = x0 + p * (col_w + margin);
        BassPlayer *pl = &g_players[p];
        color_t pc = col_player(p + 1);

        m64_gui_panel(x, y, col_w, 72,
                      RGBA32(10, 10, 24, 200), pc);
        m64_gui_text(x + 4, y + 3, pc, "P%d", p + 1);
        m64_gui_text(x + 24, y + 3, RGBA32(232, 232, 240, 255),
                     "%s", ENGINE_NAME[pl->engine]);
        m64_gui_text(x + 4, y + 15, RGBA32(180, 180, 200, 255),
                     "oct %+d", (int)pl->octave);

        // Stick viz (32x32).
        bass_stick_viz(x + 4, y + 28, 32, 32, pl->stick_x, pl->stick_y,
                       pc, RGBA32(0, 245, 212, 255));

        // Last note + variant label.
        m64_gui_text(x + 40, y + 28, RGBA32(232, 232, 240, 255),
                     "%s", pl->last_note > 0 ? note_name(pl->last_note) : "--");
        m64_gui_text(x + 40, y + 40, RGBA32(140, 140, 160, 255),
                     "%s", VARIANT_NAME[pl->last_variant]);

        // Active-voice meter for this player (count of voices on this player).
        int vp = 0;
        for (int i = 0; i < BASS_VOICES; i++)
            if (g_voices[i].active && g_voices[i].player == (uint8_t)(p + 1)) vp++;
        float frac = (float)vp / 4.0f;  // 4 voices per player = full bar
        bass_vbar(x + col_w - 12, y + 4, 8, 64, frac, pc, RGBA32(40, 40, 60, 255));
    }
}

// ── 3D: 12 spectrum cubes ─────────────────────────────────────────────────
static void draw_spectrum_cubes(float t)
{
    m64_scene_begin(&g_scene);

    for (int i = 0; i < BASS_VOICES; i++) {
        BassVoice *v = &g_voices[i];
        float amp = v->active ? v->vol_current : 0.0f;

        // Cube spacing: 12 across, centred at x=0.
        float x = (i - (BASS_VOICES - 1) * 0.5f) * 4.0f;

        g_cube_xform[i].pos = (fm_vec3_t){{ x, 0, 0 }};
        // Y scale = 0.3 + amp * 6.0 (tall bar when fully on).
        g_cube_xform[i].scale = (fm_vec3_t){{ 1.4f, 0.3f + amp * 6.0f, 1.4f }};
        g_cube_xform[i].rot_angle = 0.0f;

        // Push transform, load cube verts, draw, pop. The verts are shared
        // across all 12 cubes — only the transform differs.
        m64_transform_push(&g_cube_xform[i]);
        t3d_vert_load(g_cube_verts, 0, 8);
        m64_transform_pop();

        for (int t2 = 0; t2 < 12; t2++)
            t3d_tri_draw(CUBE_TRIS[t2][0], CUBE_TRIS[t2][1], CUBE_TRIS[t2][2]);
        t3d_tri_sync();
    }
}

// ── Boot ──────────────────────────────────────────────────────────────────
static T3DVertPacked *make_cube_verts(void)
{
    T3DVertPacked *v = malloc_uncached(sizeof(T3DVertPacked) * 4);
    const int16_t s = 14;
    struct { int16_t x, y, z; uint32_t rgba; } c[8] = {
        { -s, -s, -s, 0xFFFFFFFF }, {  s, -s, -s, 0xFFFFFFFF },
        {  s,  s, -s, 0xFFFFFFFF }, { -s,  s, -s, 0xFFFFFFFF },
        { -s, -s,  s, 0xFFFFFFFF }, {  s, -s,  s, 0xFFFFFFFF },
        {  s,  s,  s, 0xFFFFFFFF }, { -s,  s,  s, 0xFFFFFFFF },
    };
    for (int i = 0; i < 8; i += 2) {
        fm_vec3_t na = {{ (float)c[i].x,   (float)c[i].y,   (float)c[i].z   }};
        fm_vec3_t nb = {{ (float)c[i+1].x, (float)c[i+1].y, (float)c[i+1].z }};
        fm_vec3_norm(&na, &na);
        fm_vec3_norm(&nb, &nb);
        v[i / 2] = (T3DVertPacked){
            .posA = { c[i].x,   c[i].y,   c[i].z   },
            .rgbaA = c[i].rgba,
            .normA = t3d_vert_pack_normal(&na),
            .posB = { c[i+1].x, c[i+1].y, c[i+1].z },
            .rgbaB = c[i+1].rgba,
            .normB = t3d_vert_pack_normal(&nb),
        };
    }
    return v;
}

static void load_wavetables(void)
{
    static const char *engines[BASS_ENGINE_COUNT] = {
        "heavy", "sub", "growl", "industrial"
    };
    static const char *variants[BASS_VARIANT_COUNT] = {
        "bright", "dark", "clean", "driven"
    };
    for (int e = 0; e < BASS_ENGINE_COUNT; e++) {
        for (int v = 0; v < BASS_VARIANT_COUNT; v++) {
            char path[64];
            snprintf(path, sizeof(path), "rom:/sfx/bass_%s_%s.wav64",
                     engines[e], variants[v]);
            g_wt[e][v] = m64_sfx_load(path);
            if (g_wt[e][v] < 0) {
                debugf("bass-synth: failed to load %s\n", path);
            }
        }
    }
}

int main(void)
{
    m64_engine_init(RESOLUTION_320x240);
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);
    m64_input_init();
    m64_gui_init();

    m64_audio_init((M64AudioConfig){
        .sample_rate = SAMPLE_RATE,
        .latency = 0.16f,
        .sfx_channels = BASS_VOICES,
        .music_channels = 0,
    });

    load_wavetables();

    // Init scene + camera for the cube field.
    m64_scene_init(&g_scene);
    g_scene.cam_pos = (fm_vec3_t){{ 0, 8, -60 }};
    g_scene.cam_target = (fm_vec3_t){{ 0, 0, 0 }};
    g_scene.far_z = 300.0f;
    m64_scene_update(&g_scene);

    // Init the 12 cube transforms (one per voice slot).
    for (int i = 0; i < BASS_VOICES; i++) {
        m64_transform_init(&g_cube_xform[i]);
        g_cube_xform[i].scale = (fm_vec3_t){{ 1.4f, 0.3f, 1.4f }};
    }
    g_cube_verts = make_cube_verts();

    for (int i = 0; i < BASS_VOICES; i++) {
        g_voices[i] = (BassVoice){ .channel = (uint8_t)i };
    }

    uint32_t last_ticks = get_ticks();
    float t = 0.0f;

    for (;;) {
        m64_input_update();

        if (g_menu_open) {
            handle_menu_input();
        } else {
            handle_play_input();
            // Toggle menu with Start edge.
            const M64Input *in = m64_input_get(1);
            if (in && (in->edges & M64_BTN_START)) g_menu_open = 1;
        }

        uint32_t now = get_ticks();
        float dt_ms = (float)TICKS_DISTANCE(last_ticks, now)
                      / (float)TICKS_PER_SECOND * 1000.0f;
        last_ticks = now;
        if (dt_ms > 100.0f) dt_ms = 100.0f;  // clamp huge stalls
        t += dt_ms / 1000.0f;

        update_voices(dt_ms);

        m64_frame_begin();
        draw_spectrum_cubes(t);
        m64_gui_begin();
        if (g_menu_open) draw_menu();
        draw_activity_overlay();
        m64_gui_end();
        m64_frame_end();

        m64_audio_update();
    }
}