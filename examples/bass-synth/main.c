// SPDX-License-Identifier: MIT
//
// Bass synth — a 4-controller collaborative bass ROM, modelled on a Moog
// Subsequent 37 rather than on a synth-action game.
//
//   1. Wavetables   12 single-cycle 256-sample mono wavs (4 engines x {body
//                   bright, body dark, sub}), baked as looping VADPCM and
//                   pitched live by the RSP mixer. A held note owns TWO mixer
//                   channels — body (the bright or dark table, picked by stick
//                   X at note-on) and sub (an octave below) — so 12 channels
//                   are 6 notes.
//   2. ADSR         per engine, linear, one multiply per stage per frame.
//   3. Allocator    6 note slots, fixed channel pair per slot; steals a
//                   releasing note first, then the lowest.
//   4. Input        12 buttons = 12 scale steps per player (chromatic, or a
//                   pentatonic/blues run over two octaves); Z sustains; stick X
//                   brightness, stick Y sub drive, stick magnitude and C-stick
//                   Y vibrato, C-stick X +-1 semitone bend. Highest held
//                   button sounds; legato glides, retrigger re-attacks.
//   5. Intro        an 8.5 s scripted riff through the same note_on/note_off
//                   as the pads; any button skips. After it, an attract tape
//                   plays a bassline on port 1 when every pad is idle.
//   6. Patch        Start opens a paged editor (one page per player plus a
//                   global page); closing it SAVES through kiln_store — SD
//                   card, else the 32 KB save chip — and the HUD says which
//                   backend took it and whether it worked. Loaded at boot.
//   7. Visual       a fixed arc of 24 bars, 6 per player in the player's
//                   colour: bar 0 is the sub layer, bars 1-5 the body's first
//                   five harmonics for that engine's wavetable, scaled by the
//                   live envelopes, clamped. It is a model of the tables (the
//                   RSP does not hand back per-channel samples); the scope
//                   under the status line is the real mixed output, from
//                   kiln_audio's tap.
//
// ── What was broken ─────────────────────────────────────────────────────
//   * SILENT. note_on never started a channel: there was no kiln_sfx_play
//     anywhere, so every frequency and volume write went to idle channels.
//   * It would have asserted the moment it did sound: a note's body plays at
//     freq x 256 Hz, libdragon's mixer asserts above a channel's limit, and
//     the limit defaults to the 32 kHz output — P2's first intro note (E2,
//     MIDI 40, 42 kHz) is over it. The 12 channels' limits are raised to 192 kHz and
//     the octave range capped so no note can pass it.
//   * Legato never changed pitch: it updated the note number but not
//     freq_hz, so a glide glided to the note already sounding.
//   * portamento_ms was a uint8_t with a menu range to 500: 300 stored as 44.
//   * Z was a note button AND the sustain modifier.
//   * The patch "saved" with fopen("rom:/bass-synth.pat", "wb") — read-only
//     DragonFS — so it never saved, silently, and it saved when the menu
//     OPENED, before any edit. The header said eepromfs, the README said DFS.
//   * The menu was 26 rows x 14 px on a 240 px screen; the status line was 58
//     characters in a 53-character strip; each player's volume row wrote the
//     shared master gain.
//   * midi_to_hz used powf, the LFO sinf, the stick sqrtf: libm on the
//     VR4300 every frame for every note. Now a table, fm_sinf and squares.

#include <libdragon.h>

#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_audio.h>
#include <kiln/kiln_prim.h>
#include <kiln/kiln_store.h>

#include <string.h>

enum { JUMP_NONE, JUMP_PATCH };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W    320
#define SCREEN_H    240
#define SAMPLE_RATE 32000

#define BASS_NOTES  6
#define BASS_CHANS  (BASS_NOTES * 2)
#define WT_LEN      256
#define NOTE_KEYS   12
#define BASE_MIDI   28                 // E1 (41 Hz, MIDI 60 = C4), a 4-string bass's open low string
#define OCTAVE_MIN  -12
#define OCTAVE_MAX  36
// Highest body rate: MIDI 28+36+27 (a pentatonic run's top) would be 91; notes
// are clamped to MIDI 76 (E5, 659 Hz), x256 = 169 kHz, x bend and vibrato
// about 180 kHz. The limit sits above that.
#define MIDI_MAX    76
#define CH_MAX_FREQ 192000.0f

#define NPLAYERS 4
#define SCOPE_N  96
#define ARC_BARS 6                     // per player

// ── Scales: button index -> semitones above the player's root ────────────
typedef enum { SCALE_CHROMATIC, SCALE_MAJOR_PENT, SCALE_MINOR_PENT, SCALE_BLUES, SCALE_COUNT } Scale;
static const int8_t SCALE_STEPS[SCALE_COUNT][NOTE_KEYS] = {
    [SCALE_CHROMATIC]  = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 },
    [SCALE_MAJOR_PENT] = { 0, 2, 4, 7, 9, 12, 14, 16, 19, 21, 24, 26 },
    [SCALE_MINOR_PENT] = { 0, 3, 5, 7, 10, 12, 15, 17, 19, 22, 24, 27 },
    [SCALE_BLUES]      = { 0, 3, 5, 6, 7, 10, 12, 15, 17, 18, 19, 22 },
};
static const char *const SCALE_NAME[SCALE_COUNT] = { "CHROM", "MAJP5", "MINP5", "BLUES" };

// Z is not here: it is sustain.
static const uint32_t NOTE_BUTTONS[NOTE_KEYS] = {
    KILN_BTN_DU, KILN_BTN_DL, KILN_BTN_DD, KILN_BTN_DR,
    KILN_BTN_CL, KILN_BTN_CD, KILN_BTN_CR, KILN_BTN_CU,
    KILN_BTN_L,  KILN_BTN_B,  KILN_BTN_A,  KILN_BTN_R,
};

// ── Engines ───────────────────────────────────────────────────────────────
typedef enum { BASS_HEAVY, BASS_SUB, BASS_GROWL, BASS_INDUSTRIAL, BASS_ENGINE_COUNT } BassEngine;
static const char *const ENGINE_NAME[BASS_ENGINE_COUNT] = { "HEAVY", "SUB", "GROWL", "INDUST" };

typedef struct {
    float attack_ms, decay_ms, sustain_lvl, release_ms;
    float sub_gain, body_gain;
    int   wt_body_bright, wt_body_dark, wt_sub;
    // Relative harmonic amplitudes 1..5 of the bright and dark body tables,
    // for the arc (tools/gen_bass_wav.py's recipes, rounded).
    float bright[5], dark[5];
} BassEngineDef;

static BassEngineDef g_engine[BASS_ENGINE_COUNT] = {
    [BASS_HEAVY]      = { 5, 180, 0.70f, 120, 0.30f, 0.85f, -1, -1, -1,
                          { 1.00f, 0.50f, 0.33f, 0.25f, 0.20f }, { 1.00f, 0.30f, 0.12f, 0.05f, 0.02f } },
    [BASS_SUB]        = { 8, 300, 0.80f, 200, 0.65f, 0.70f, -1, -1, -1,
                          { 1.00f, 0.05f, 0.33f, 0.03f, 0.02f }, { 1.00f, 0.02f, 0.15f, 0.01f, 0.01f } },
    [BASS_GROWL]      = { 4, 220, 0.60f, 140, 0.40f, 0.85f, -1, -1, -1,
                          { 0.80f, 0.70f, 0.55f, 0.45f, 0.30f }, { 0.90f, 0.45f, 0.25f, 0.12f, 0.06f } },
    [BASS_INDUSTRIAL] = { 3, 140, 0.50f,  90, 0.45f, 0.90f, -1, -1, -1,
                          { 1.00f, 0.08f, 0.33f, 0.08f, 0.20f }, { 1.00f, 0.04f, 0.18f, 0.04f, 0.07f } },
};

// ── Notes ─────────────────────────────────────────────────────────────────
typedef enum { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE } EnvState;

typedef struct {
    uint8_t  active, held, sustained;   // sustained: released while Z was down
    uint8_t  body_ch, sub_ch, player, engine;
    int8_t   note;
    float    freq_hz;                   // target, before portamento/bend/LFO
    float    glide;                     // current body rate, portamento state
    float    age_ms;
    EnvState env;
    float    env_level;
    float    bright;                    // 1 = bright table, 0 = dark
} BassNote;

static BassNote g_notes[BASS_NOTES];

typedef struct {
    int8_t   engine, octave;
    uint8_t  legato, scale;
    uint16_t portamento_ms;             // was uint8_t with a menu range to 500
    float    volume;                    // this player's own level, 0..1
    float    stick_x, stick_y, stick_mag2, cstick_x, cstick_y;
    uint8_t  z_held;
    int8_t   last_note, held_button;
} BassPlayer;

static BassPlayer g_players[NPLAYERS];
static const float PLAYER_PAN[NPLAYERS] = { 0.30f, 0.44f, 0.56f, 0.70f };
static const uint32_t PLAYER_RGB[NPLAYERS] = { 0x00E5FFFF, 0x4CFF82FF, 0xFFD94CFF, 0xFF4C6AFF };

static float g_master_gain = 0.75f;
static float g_lfo_rate_hz = 5.0f;
static float g_lfo_phase = 0.0f;
static float g_midi_hz[128];

static color_t rgb(uint32_t c) { return RGBA32(c >> 24, (c >> 16) & 0xFF, (c >> 8) & 0xFF, 0xFF); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// Equal temperament by repeated multiplication from A4: no powf.
static void build_midi_table(void)
{
    const float semitone = 1.0594631f;
    g_midi_hz[69] = 440.0f;
    for (int n = 70; n < 128; n++) g_midi_hz[n] = g_midi_hz[n - 1] * semitone;
    for (int n = 68; n >= 0; n--)  g_midi_hz[n] = g_midi_hz[n + 1] / semitone;
}

static int midi_for(const BassPlayer *pl, int key)
{
    int m = BASE_MIDI + pl->octave + SCALE_STEPS[pl->scale][key];
    return m < 0 ? 0 : m > MIDI_MAX ? MIDI_MAX : m;
}

// 2^(semi/12) for semi in [-1, 1], linearised on each side (error < 0.2%).
static float bend_ratio(float semi)
{
    return semi >= 0.0f ? 1.0f + semi * 0.0594631f : 1.0f + semi * 0.0561257f;
}

// ── Output tap: the mixed buffer, for the scope ──────────────────────────
static int16_t g_scope[SCOPE_N];
static int g_scope_peak;

static void scope_tap(const int16_t *s, int frames, void *ctx)
{
    (void)ctx;
    int start = 0;
    for (int i = 1; i < frames / 2; i++)
        if (s[(i - 1) * 2] < 0 && s[i * 2] >= 0) { start = i; break; }
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

// ── Voice allocation ─────────────────────────────────────────────────────
static int note_find_victim(void)
{
    int best = -1, best_score = 0x7fffffff;
    for (int i = 0; i < BASS_NOTES; i++) {
        const BassNote *n = &g_notes[i];
        if (!n->active) return i;
        // Lower is more stealable: releasing first, then oldest.
        int score = (n->env == ENV_RELEASE ? 0 : 100000) - (int)n->age_ms;
        if (score < best_score) { best_score = score; best = i; }
    }
    return best;
}

static void note_on(int player, int midi)
{
    BassPlayer *pl = &g_players[player];
    const BassEngineDef *eng = &g_engine[pl->engine];

    int existing = -1;
    for (int i = 0; i < BASS_NOTES; i++)
        if (g_notes[i].active && g_notes[i].held && g_notes[i].player == player) { existing = i; break; }

    if (existing >= 0 && pl->legato) {
        // Glide: the channels keep playing and the envelope keeps its stage;
        // the TARGET moves, and update_voices slides `glide` towards it.
        BassNote *n = &g_notes[existing];
        n->note = (int8_t)midi;
        n->freq_hz = g_midi_hz[midi];
        n->age_ms = 0.0f;
        pl->last_note = (int8_t)midi;
        return;
    }

    const int slot = existing >= 0 ? existing : note_find_victim();
    BassNote *n = &g_notes[slot];
    const int retrigger = n->active;
    const float level = retrigger ? n->env_level : 0.0f;     // no click
    const float glide_from = retrigger ? n->glide : g_midi_hz[midi] * WT_LEN;

    n->active = 1;
    n->held = 1;
    n->sustained = 0;
    n->player = (uint8_t)player;
    n->engine = (uint8_t)pl->engine;
    n->note = (int8_t)midi;
    n->freq_hz = g_midi_hz[midi];
    n->glide = pl->portamento_ms > 0 ? glide_from : g_midi_hz[midi] * WT_LEN;
    n->age_ms = 0.0f;
    n->env = ENV_ATTACK;
    n->env_level = level;
    n->bright = clampf((pl->stick_x + 1.0f) * 0.5f, 0.0f, 1.0f);
    n->body_ch = (uint8_t)(slot * 2);
    n->sub_ch = (uint8_t)(slot * 2 + 1);

    // START THE CHANNELS. The old code never did, and was silent. Volume 0:
    // the envelope brings it up on this frame's update, before the mix.
    const int body_wt = n->bright >= 0.5f ? eng->wt_body_bright : eng->wt_body_dark;
    kiln_sfx_play_ex(body_wt, n->body_ch, 2, 0.0f, PLAYER_PAN[player]);
    kiln_sfx_play_ex(eng->wt_sub, n->sub_ch, 2, 0.0f, PLAYER_PAN[player]);
    kiln_sfx_set_freq(n->body_ch, n->glide);
    kiln_sfx_set_freq(n->sub_ch, n->glide * 0.5f);
    pl->last_note = (int8_t)midi;
}

static void note_off(int player)
{
    int found = -1;
    float newest = 1e9f;
    for (int i = 0; i < BASS_NOTES; i++) {
        const BassNote *n = &g_notes[i];
        if (!n->active || !n->held || n->player != player) continue;
        if (n->age_ms < newest) { newest = n->age_ms; found = i; }
    }
    if (found < 0) return;
    BassNote *n = &g_notes[found];
    n->held = 0;
    if (g_players[player].z_held) n->sustained = 1;
    else n->env = ENV_RELEASE;
}

static void release_sustained(int player)
{
    for (int i = 0; i < BASS_NOTES; i++) {
        BassNote *n = &g_notes[i];
        if (n->active && n->player == player && n->sustained) { n->sustained = 0; n->env = ENV_RELEASE; }
    }
}

static void release_all(void)
{
    for (int i = 0; i < BASS_NOTES; i++) {
        BassNote *n = &g_notes[i];
        if (n->active) { n->held = 0; n->sustained = 0; n->env = ENV_RELEASE; }
    }
    for (int p = 0; p < NPLAYERS; p++) g_players[p].held_button = -1;
}

// ── Per-frame voice update ────────────────────────────────────────────────
static void update_voices(float dt_ms)
{
    g_lfo_phase += dt_ms * 0.001f * g_lfo_rate_hz;
    g_lfo_phase -= (float)(int)g_lfo_phase;
    const float lfo = fm_sinf(g_lfo_phase * 6.2831853f);

    for (int i = 0; i < BASS_NOTES; i++) {
        BassNote *n = &g_notes[i];
        if (!n->active) continue;
        n->age_ms += dt_ms;
        const BassEngineDef *eng = &g_engine[n->engine];
        const BassPlayer *pl = &g_players[n->player];

        switch (n->env) {
        case ENV_ATTACK:
            n->env_level += dt_ms / eng->attack_ms;
            if (n->env_level >= 1.0f) { n->env_level = 1.0f; n->env = ENV_DECAY; }
            break;
        case ENV_DECAY:
            n->env_level -= dt_ms / eng->decay_ms;
            if (n->env_level <= eng->sustain_lvl) { n->env_level = eng->sustain_lvl; n->env = ENV_SUSTAIN; }
            break;
        case ENV_SUSTAIN:
            n->env_level = eng->sustain_lvl;
            break;
        case ENV_RELEASE:
            n->env_level -= dt_ms / eng->release_ms;
            if (n->env_level <= 0.0f) {
                kiln_sfx_stop(n->body_ch);
                kiln_sfx_stop(n->sub_ch);
                memset(n, 0, sizeof *n);
                continue;
            }
            break;
        default:
            break;
        }

        // Portamento: an exponential slide of the body rate to its target.
        const float target = n->freq_hz * WT_LEN;
        if (pl->portamento_ms > 0) {
            const float k = clampf(dt_ms / (float)pl->portamento_ms, 0.0f, 1.0f);
            n->glide += (target - n->glide) * k;
        } else {
            n->glide = target;
        }

        const float vib_depth = (pl->stick_mag2 > 0.01f ? pl->stick_mag2 : 0.0f) * 0.015f
                              + clampf(pl->cstick_y, 0.0f, 1.0f) * 0.04f;
        const float rate = n->glide * bend_ratio(clampf(pl->cstick_x, -1.0f, 1.0f)) * (1.0f + lfo * vib_depth);
        kiln_sfx_set_freq(n->body_ch, clampf(rate, 1.0f, CH_MAX_FREQ));
        kiln_sfx_set_freq(n->sub_ch,  clampf(rate * 0.5f, 1.0f, CH_MAX_FREQ));

        const float level = n->env_level * pl->volume * g_master_gain;
        const float drive = 1.0f + (pl->stick_y > 0.0f ? pl->stick_y : 0.0f) * 1.2f;
        float body = level * eng->body_gain, sub = level * eng->sub_gain * drive;
        body = body / (1.0f + body);   // soft clip, no libm
        sub  = sub  / (1.0f + sub);
        const float pan = clampf(PLAYER_PAN[n->player] + (pl->stick_y > 0.0f ? pl->stick_y * 0.2f : 0.0f), 0.0f, 1.0f);
        kiln_sfx_set_vol_pan(n->body_ch, body, pan);
        kiln_sfx_set_vol_pan(n->sub_ch, sub, pan);
    }
}

// ── Live input ────────────────────────────────────────────────────────────
static void handle_play_input(void)
{
    for (int p = 0; p < NPLAYERS; p++) {
        const KilnInput *in = kiln_input_get(p + 1);
        BassPlayer *pl = &g_players[p];
        pl->stick_x = in->stick_x;
        pl->stick_y = in->stick_y;
        pl->stick_mag2 = clampf(in->stick_x * in->stick_x + in->stick_y * in->stick_y, 0.0f, 1.0f);
        pl->cstick_x = in->cstick_x;
        pl->cstick_y = in->cstick_y;

        const uint8_t z = (in->buttons & KILN_BTN_Z) ? 1 : 0;
        if (!z && pl->z_held) release_sustained(p);
        pl->z_held = z;

        // The highest held button sounds. A change of it is a new note (a
        // glide in legato), all released is a note-off.
        int top = -1;
        for (int k = 0; k < NOTE_KEYS; k++)
            if (in->buttons & NOTE_BUTTONS[k]) top = k;
        if (top != pl->held_button) {
            if (top >= 0) note_on(p, midi_for(pl, top));
            else note_off(p);
            pl->held_button = (int8_t)top;
        }
    }
}

// ── Intro ─────────────────────────────────────────────────────────────────
typedef struct { uint16_t t_ms; uint8_t player; int8_t note; uint16_t dur_ms; } IntroEvent;

// E minor, ~80 BPM; one engine per bar, then P4 and P1 together.
static const IntroEvent INTRO[] = {
    {    0, 0, 28, 350 }, {  375, 0, 31, 200 }, {  600, 0, 33, 150 }, {  775, 0, 35, 350 },
    { 1150, 0, 33, 200 }, { 1375, 0, 31, 200 },
    { 1700, 1, 40, 350 }, { 2075, 1, 41, 350 }, { 2450, 1, 43, 350 }, { 2825, 1, 45, 350 },
    { 3200, 2, 47, 150 }, { 3400, 2, 45, 150 }, { 3600, 2, 47, 150 }, { 3800, 2, 48, 250 },
    { 4100, 2, 47, 150 }, { 4300, 2, 45, 150 }, { 4500, 2, 43, 150 }, { 4700, 2, 45, 300 },
    { 5100, 3, 28, 700 }, { 5100, 0, 40, 700 }, { 5800, 3, 30, 200 }, { 6000, 0, 42, 200 },
    { 6200, 3, 33, 700 }, { 6200, 0, 45, 700 }, { 6900, 3, 35, 700 }, { 6900, 0, 47, 700 },
    { 7600, 3, 28, 850 },
};
#define INTRO_EVENTS ((int)(sizeof INTRO / sizeof INTRO[0]))
#define INTRO_TOTAL_MS 8500

static int      g_intro_active = 1;
static int      g_intro_next;
static uint16_t g_intro_off_ms[NPLAYERS];    // 0 = none pending

static void intro_step(float t_ms)
{
    while (g_intro_next < INTRO_EVENTS && INTRO[g_intro_next].t_ms <= t_ms) {
        const IntroEvent *e = &INTRO[g_intro_next++];
        note_off(e->player);                 // one intro note per player
        note_on(e->player, e->note);
        g_intro_off_ms[e->player] = (uint16_t)(e->t_ms + e->dur_ms);
    }
    for (int p = 0; p < NPLAYERS; p++) {
        if (g_intro_off_ms[p] && t_ms >= g_intro_off_ms[p]) {
            note_off(p);
            g_intro_off_ms[p] = 0;
        }
    }
}

// ── Attract: a bassline on port 1 (chromatic, octave 0: DU is E1) ────────
#define B_E  KILN_BTN_DU   // E1
#define B_G  KILN_BTN_DR   // G1
#define B_A  KILN_BTN_CD   // A1
#define B_B  KILN_BTN_CU   // B1
#define B_D  KILN_BTN_A    // D2
static const KilnInputKey BASSLINE_KEYS[] = {
    { .frame =   0, .buttons = B_E }, { .frame =  12 },
    { .frame =  15, .buttons = B_E }, { .frame =  22 },
    { .frame =  30, .buttons = B_G }, { .frame =  42 },
    { .frame =  45, .buttons = B_A }, { .frame =  57 },
    { .frame =  60, .buttons = B_E }, { .frame =  72 },
    { .frame =  75, .buttons = B_E, .sx = 70 }, { .frame =  82 },
    { .frame =  90, .buttons = B_B, .sx = 70 }, { .frame = 102 },
    { .frame = 105, .buttons = B_A }, { .frame = 117 },
    { .frame = 120, .buttons = B_E }, { .frame = 132 },
    { .frame = 135, .buttons = B_E }, { .frame = 142 },
    { .frame = 150, .buttons = B_G }, { .frame = 162 },
    { .frame = 165, .buttons = B_A, .cy = 60 }, { .frame = 177 },
    { .frame = 180, .buttons = B_D, .cx = 40 }, { .frame = 204 },
    { .frame = 210, .buttons = B_B }, { .frame = 222 },
    { .frame = 225, .buttons = B_A }, { .frame = 237 },
    { .frame = 240 },
};
static const KilnInputTape BASSLINE = { BASSLINE_KEYS, sizeof BASSLINE_KEYS / sizeof BASSLINE_KEYS[0], 0 };

// Jump ROM .#bass-synth-patch: in the editor, turn P1's engine one step, close
// (which SAVES), reopen — then hold, so the menu footer shows the backend and
// the save result with no pad attached.
static const KilnInputKey PATCH_KEYS[] = {
    { .frame =  0 },
    { .frame = 30, .buttons = KILN_BTN_DR },    { .frame = 34 },
    { .frame = 60, .buttons = KILN_BTN_START }, { .frame = 64 },
    { .frame = 90, .buttons = KILN_BTN_START }, { .frame = 94 },
    { .frame = 95 },
};
static const KilnInputTape PATCH_TAPE = { PATCH_KEYS, sizeof PATCH_KEYS / sizeof PATCH_KEYS[0], KILN_INPUT_NO_LOOP };

// ── Patch: kiln_store ─────────────────────────────────────────────────────
#define PATCH_NAME    "BASSPAT"
#define PATCH_VERSION 2

typedef struct __attribute__((packed)) {
    uint8_t engine[NPLAYERS];
    int8_t  octave[NPLAYERS];
    uint8_t legato[NPLAYERS];
    uint8_t portamento_div10[NPLAYERS];   // ms / 10, 0..50
    uint8_t scale[NPLAYERS];
    uint8_t volume_pct[NPLAYERS];
    uint8_t master_pct;
    uint8_t lfo_x2;                        // Hz x 2
} PatchState;

static int      g_store_status = KILN_STORE_ENOENT;
static uint32_t g_store_shown_frame;       // frame the last save/load landed
static const char *g_store_what = "load";

static void save_patch(uint32_t frame)
{
    PatchState ps;
    memset(&ps, 0, sizeof ps);
    for (int i = 0; i < NPLAYERS; i++) {
        ps.engine[i] = (uint8_t)g_players[i].engine;
        ps.octave[i] = g_players[i].octave;
        ps.legato[i] = g_players[i].legato;
        ps.portamento_div10[i] = (uint8_t)(g_players[i].portamento_ms / 10);
        ps.scale[i] = g_players[i].scale;
        ps.volume_pct[i] = (uint8_t)(g_players[i].volume * 100.0f + 0.5f);
    }
    ps.master_pct = (uint8_t)(g_master_gain * 100.0f + 0.5f);
    ps.lfo_x2 = (uint8_t)(g_lfo_rate_hz * 2.0f + 0.5f);
    g_store_status = kiln_store_write(PATCH_NAME, PATCH_VERSION, &ps, sizeof ps);
    g_store_what = "save";
    g_store_shown_frame = frame;
}

static void load_patch(void)
{
    PatchState ps;
    uint32_t len = 0;
    g_store_status = kiln_store_read(PATCH_NAME, PATCH_VERSION, &ps, sizeof ps, &len);
    g_store_what = "load";
    if (g_store_status != KILN_STORE_OK || len != sizeof ps) return;
    for (int i = 0; i < NPLAYERS; i++) {
        g_players[i].engine = (int8_t)(ps.engine[i] % BASS_ENGINE_COUNT);
        g_players[i].octave = (int8_t)clampf(ps.octave[i], OCTAVE_MIN, OCTAVE_MAX);
        g_players[i].legato = ps.legato[i] ? 1 : 0;
        g_players[i].portamento_ms = (uint16_t)(ps.portamento_div10[i] > 50 ? 500 : ps.portamento_div10[i] * 10);
        g_players[i].scale = (uint8_t)(ps.scale[i] % SCALE_COUNT);
        g_players[i].volume = clampf(ps.volume_pct[i] / 100.0f, 0.0f, 1.0f);
    }
    g_master_gain = clampf(ps.master_pct / 100.0f, 0.0f, 1.0f);
    g_lfo_rate_hz = clampf(ps.lfo_x2 / 2.0f, 0.5f, 12.0f);
}

// ── Menu ──────────────────────────────────────────────────────────────────
enum { ROW_ENGINE, ROW_OCTAVE, ROW_LEGATO, ROW_PORTA, ROW_SCALE, ROW_VOLUME, PLAYER_ROWS };
enum { ROW_MASTER, ROW_LFO, GLOBAL_ROWS };
#define PAGE_GLOBAL NPLAYERS

static int g_menu_open, g_menu_page, g_menu_row;

static void menu_apply(int dir)
{
    if (g_menu_page == PAGE_GLOBAL) {
        if (g_menu_row == ROW_MASTER) g_master_gain = clampf(g_master_gain + dir * 0.05f, 0.0f, 1.0f);
        else g_lfo_rate_hz = clampf(g_lfo_rate_hz + dir * 0.5f, 0.5f, 12.0f);
        return;
    }
    BassPlayer *pl = &g_players[g_menu_page];
    switch (g_menu_row) {
    case ROW_ENGINE: pl->engine = (int8_t)((pl->engine + dir + BASS_ENGINE_COUNT) % BASS_ENGINE_COUNT); break;
    case ROW_OCTAVE: pl->octave = (int8_t)clampf(pl->octave + dir * 12, OCTAVE_MIN, OCTAVE_MAX); break;
    case ROW_LEGATO: pl->legato = !pl->legato; break;
    case ROW_PORTA:  pl->portamento_ms = (uint16_t)clampf((float)pl->portamento_ms + dir * 50.0f, 0.0f, 500.0f); break;
    case ROW_SCALE:  pl->scale = (uint8_t)((pl->scale + dir + SCALE_COUNT) % SCALE_COUNT); break;
    case ROW_VOLUME: pl->volume = clampf(pl->volume + dir * 0.1f, 0.0f, 1.0f); break;
    default: break;
    }
}

static void handle_menu_input(uint32_t frame)
{
    const KilnInput *in = kiln_input_get(1);
    const int rows = g_menu_page == PAGE_GLOBAL ? GLOBAL_ROWS : PLAYER_ROWS;
    if (in->edges & KILN_BTN_DU) g_menu_row = (g_menu_row + rows - 1) % rows;
    if (in->edges & KILN_BTN_DD) g_menu_row = (g_menu_row + 1) % rows;
    if (in->edges & KILN_BTN_DR) menu_apply(+1);
    if (in->edges & KILN_BTN_DL) menu_apply(-1);
    int page = 0;
    if (in->edges & (KILN_BTN_R | KILN_BTN_CR)) page = 1;
    if (in->edges & (KILN_BTN_L | KILN_BTN_CL)) page = -1;
    if (page) {
        g_menu_page = (g_menu_page + page + NPLAYERS + 1) % (NPLAYERS + 1);
        const int new_rows = g_menu_page == PAGE_GLOBAL ? GLOBAL_ROWS : PLAYER_ROWS;
        if (g_menu_row >= new_rows) g_menu_row = new_rows - 1;
    }
    if (in->edges & KILN_BTN_START) {
        g_menu_open = 0;
        save_patch(frame);   // on CLOSE, after the edits — it used to be on open
    }
}

// ── 2D ────────────────────────────────────────────────────────────────────
static const color_t INK   = RGBA32(0xE8, 0xE8, 0xF0, 0xFF);
static const color_t DIM   = RGBA32(0x90, 0x98, 0xB0, 0xFF);
static const color_t TEAL  = RGBA32(0x00, 0xF5, 0xD4, 0xFF);
static const color_t PANEL = RGBA32(0x0C, 0x10, 0x1C, 0xFF);
static const color_t WELL  = RGBA32(0x2A, 0x2A, 0x3E, 0xFF);
static const color_t BAD   = RGBA32(0xFF, 0x4C, 0x6A, 0xFF);

static const char *note_name(int midi, char *buf, int cap)
{
    static const char *const N[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    snprintf(buf, (size_t)cap, "%s%d", N[midi % 12], midi / 12 - 1);
    return buf;
}

static const char *env_name(EnvState e)
{
    switch (e) {
    case ENV_ATTACK:  return "ATK";
    case ENV_DECAY:   return "DEC";
    case ENV_SUSTAIN: return "SUS";
    case ENV_RELEASE: return "REL";
    default:          return "---";
    }
}

// "sram ok", "rom readonly" — the backend that took the patch and what it said.
static void store_line(char *buf, int cap)
{
    snprintf(buf, (size_t)cap, "%s %s %s", g_store_what, kiln_store_kind_name(),
             kiln_store_status_name(g_store_status));
}

static void draw_status(int active)
{
    kiln_gui_panel(4, 4, SCREEN_W - 8, 18, PANEL, RGBA32(0x8B, 0x5C, 0xF6, 0xFF));
    // 50 characters at most: the strip is 312 px of a 6 px font.
    kiln_gui_text(10, 16, INK, "BASS  notes %d/%d  mst %3d%%  lfo %4.1f", active, BASS_NOTES,
                  (int)(g_master_gain * 100.0f + 0.5f), (double)g_lfo_rate_hz);
    kiln_gui_text(SCREEN_W - 64, 16, kiln_store_writable() ? TEAL : BAD, "save %s", kiln_store_kind_name());
}

static void draw_scope(void)
{
    const int y = 26;
    kiln_gui_panel(4, y, SCREEN_W - 8, 26, PANEL, RGBA32(0x3A, 0x40, 0x5C, 0xFF));
    kiln_gui_text(10, y + 11, DIM, "mix");
    kiln_gui_bar(10, y + 15, 24, 5, g_scope_peak / 32768.0f, RGBA32(0xFF, 0xD9, 0x4C, 0xFF), WELL);
    const int mid = y + 13;
    for (int i = 1; i < SCOPE_N; i++) {
        const int x0 = 42 + (i - 1) * 268 / SCOPE_N, x1 = 42 + i * 268 / SCOPE_N;
        const int y0 = mid - (int)clampf(g_scope[i - 1] / 1200.0f, -9, 9);
        const int y1 = mid - (int)clampf(g_scope[i] / 1200.0f, -9, 9);
        kiln_gui_line(x0, y0, x1, y1, 1, TEAL);
    }
}

static void draw_players(void)
{
    const int col_w = 76, y = SCREEN_H - 78;
    for (int p = 0; p < NPLAYERS; p++) {
        const int x = 4 + p * (col_w + 2);
        const BassPlayer *pl = &g_players[p];
        const color_t pc = rgb(PLAYER_RGB[p]);
        char nb[8];

        int newest = -1, voices = 0;
        for (int i = 0; i < BASS_NOTES; i++) {
            if (!g_notes[i].active || g_notes[i].player != p) continue;
            voices++;
            if (newest < 0 || g_notes[i].age_ms < g_notes[newest].age_ms) newest = i;
        }

        kiln_gui_panel(x, y, col_w, 74, PANEL, pc);
        kiln_gui_text(x + 4, y + 12, pc, "P%d %s", p + 1, ENGINE_NAME[pl->engine]);
        kiln_gui_text(x + 4, y + 24, DIM, "oct%+d v%d", pl->octave, (int)(pl->volume * 100.0f + 0.5f));
        kiln_gui_text(x + 4, y + 36, DIM, "%s %s", pl->legato ? "leg" : "rtr", SCALE_NAME[pl->scale]);
        kiln_gui_text(x + 4, y + 48, INK, "%-3s %s", pl->last_note >= 0 ? note_name(pl->last_note, nb, sizeof nb) : "--",
                      newest >= 0 ? env_name(g_notes[newest].env) : "---");
        kiln_gui_bar(x + 4, y + 54, col_w - 22, 5, newest >= 0 ? g_notes[newest].env_level : 0.0f, pc, WELL);

        // Stick, as a dot in a box.
        const int bx = x + col_w - 16, by = y + 4;
        kiln_gui_rect(bx, by, 12, 12, WELL);
        kiln_gui_rect(bx + 5 + (int)(pl->stick_x * 4.0f), by + 5 - (int)(pl->stick_y * 4.0f), 2, 2, TEAL);
        // Voices in use by this player.
        for (int v = 0; v < voices && v < 6; v++) kiln_gui_rect(bx + 1 + v * 2, by + 16, 1, 4, pc);

        kiln_gui_text(x + 4, y + 70, DIM, "b%+.1f m%d", (double)pl->cstick_x, (int)(clampf(pl->cstick_y, 0, 1) * 100.0f));
    }
}

static void draw_menu(uint32_t frame)
{
    const int x = 16, y = 30, w = SCREEN_W - 32;
    const int page_player = g_menu_page < NPLAYERS;
    const color_t accent = page_player ? rgb(PLAYER_RGB[g_menu_page]) : RGBA32(0xFF, 0xDC, 0x64, 0xFF);
    const int rows = page_player ? PLAYER_ROWS : GLOBAL_ROWS;

    kiln_gui_panel(x, y, w, 30 + rows * 13 + 16, PANEL, accent);
    if (page_player) kiln_gui_text(x + 6, y + 12, accent, "PATCH  P%d  %d/5", g_menu_page + 1, g_menu_page + 1);
    else kiln_gui_text(x + 6, y + 12, accent, "PATCH  GLOBAL  5/5");
    kiln_gui_text(x + 132, y + 12, DIM, "L/R page");
    kiln_gui_text(x + 6, y + 24, DIM, "up/dn row  left/rt value");

    for (int r = 0; r < rows; r++) {
        const int ry = y + 30 + r * 13;
        const int sel = r == g_menu_row;
        if (sel) kiln_gui_rect(x + 3, ry, w - 6, 12, RGBA32(0x28, 0x3C, 0x50, 0xFF));
        char label[24], value[24];
        if (page_player) {
            const BassPlayer *pl = &g_players[g_menu_page];
            static const char *const L[PLAYER_ROWS] = { "engine", "octave", "legato", "portamento", "scale", "volume" };
            snprintf(label, sizeof label, "%s", L[r]);
            switch (r) {
            case ROW_ENGINE: snprintf(value, sizeof value, "%s", ENGINE_NAME[pl->engine]); break;
            case ROW_OCTAVE: snprintf(value, sizeof value, "%+d st", pl->octave); break;
            case ROW_LEGATO: snprintf(value, sizeof value, "%s", pl->legato ? "on" : "off"); break;
            case ROW_PORTA:  snprintf(value, sizeof value, "%d ms", pl->portamento_ms); break;
            case ROW_SCALE:  snprintf(value, sizeof value, "%s", SCALE_NAME[pl->scale]); break;
            default:         snprintf(value, sizeof value, "%d%%", (int)(pl->volume * 100.0f + 0.5f)); break;
            }
        } else {
            snprintf(label, sizeof label, "%s", r == ROW_MASTER ? "master gain" : "LFO rate");
            if (r == ROW_MASTER) snprintf(value, sizeof value, "%d%%", (int)(g_master_gain * 100.0f + 0.5f));
            else snprintf(value, sizeof value, "%.1f Hz", (double)g_lfo_rate_hz);
        }
        kiln_gui_text(x + 10, ry + 10, sel ? INK : DIM, "%s", label);
        kiln_gui_text(x + 150, ry + 10, sel ? accent : INK, "%s", value);
    }

    char line[48];
    store_line(line, sizeof line);
    const int fy = y + 30 + rows * 13 + 10;
    const int ok = g_store_status == KILN_STORE_OK || (g_store_status == KILN_STORE_ENOENT && !strcmp(g_store_what, "load"));
    kiln_gui_text(x + 6, fy, kiln_store_writable() && ok ? TEAL : BAD, "Start: save+close  %s", line);
    (void)frame;
}

// ── 3D: the arc ───────────────────────────────────────────────────────────
static float bar_level(int player, int bar)
{
    float amp = 0.0f;
    for (int i = 0; i < BASS_NOTES; i++) {
        const BassNote *n = &g_notes[i];
        if (!n->active || n->player != player) continue;
        const BassEngineDef *eng = &g_engine[n->engine];
        const float lvl = n->env_level * g_players[player].volume;
        if (bar == 0) {
            amp += lvl * eng->sub_gain * 1.4f;
        } else {
            const float h = eng->bright[bar - 1] * n->bright + eng->dark[bar - 1] * (1.0f - n->bright);
            amp += lvl * eng->body_gain * h;
        }
    }
    return clampf(amp, 0.0f, 1.0f);    // clamped: never taller than the frame
}

static void load_wavetables(void)
{
    static const char *const E[BASS_ENGINE_COUNT] = { "heavy", "sub", "growl", "industrial" };
    for (int e = 0; e < BASS_ENGINE_COUNT; e++) {
        char path[64];
        snprintf(path, sizeof path, "rom:/sfx/bass_%s_body_bright.wav64", E[e]);
        g_engine[e].wt_body_bright = kiln_sfx_load(path);
        snprintf(path, sizeof path, "rom:/sfx/bass_%s_body_dark.wav64", E[e]);
        g_engine[e].wt_body_dark = kiln_sfx_load(path);
        snprintf(path, sizeof path, "rom:/sfx/bass_%s_sub.wav64", E[e]);
        g_engine[e].wt_sub = kiln_sfx_load(path);
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
        .sample_rate = SAMPLE_RATE, .latency = 0.16f, .sfx_channels = BASS_CHANS, .music_channels = 0,
    });
    // A body plays at note Hz x 256; the default limit (32 kHz) is about B1.
    for (int ch = 0; ch < BASS_CHANS; ch++) mixer_ch_set_limits(ch, 16, CH_MAX_FREQ, 0);
    kiln_audio_set_tap(scope_tap, NULL);

    build_midi_table();
    load_wavetables();
    for (int p = 0; p < NPLAYERS; p++) {
        g_players[p] = (BassPlayer){
            .engine = (int8_t)p, .octave = (int8_t)(p * 12), .legato = 1, .scale = SCALE_CHROMATIC,
            .portamento_ms = 120, .volume = 1.0f, .last_note = -1, .held_button = -1,
        };
    }
    kiln_store_init(KILN_STORE_CART_SD);
    load_patch();   // defaults stand when there is no patch yet

    KilnScene scene;
    kiln_scene_init(&scene);
    kiln_prim_stage(&scene, RGBA32(0x14, 0x10, 0x24, 0xFF), 150.0f, 320.0f);
    scene.fov_deg = 60.0f;
    scene.near_z = 10.0f;
    scene.far_z = 320.0f;

    KilnPrim floor_prim, bar_lit[NPLAYERS], bar_dim[NPLAYERS], pad[NPLAYERS];
    kiln_prim_floor(&floor_prim, 180.0f, 16, kiln_prim_rgba(0x40, 0x3C, 0x5C), kiln_prim_rgba(0x34, 0x30, 0x4E));
    for (int p = 0; p < NPLAYERS; p++) {
        const uint32_t c = PLAYER_RGB[p];
        kiln_prim_box(&bar_lit[p], (fm_vec3_t){{ 0, 10, 0 }}, (fm_vec3_t){{ 3, 10, 3 }},
                      kiln_prim_shade(c, 1.25f), c, kiln_prim_shade(c, 0.5f));
        kiln_prim_box(&bar_dim[p], (fm_vec3_t){{ 0, 10, 0 }}, (fm_vec3_t){{ 3, 10, 3 }},
                      kiln_prim_shade(c, 0.5f), kiln_prim_shade(c, 0.3f), kiln_prim_shade(c, 0.2f));
        kiln_prim_box(&pad[p], (fm_vec3_t){{ 0, 1, 0 }}, (fm_vec3_t){{ 22, 1, 7 }},
                      kiln_prim_shade(c, 0.35f), kiln_prim_shade(c, 0.25f), kiln_prim_rgba(0x10, 0x10, 0x18));
    }

    // The arc: 24 bars on a fixed 110-degree sweep in front of a fixed
    // camera, player 1 on the left. Fixed so that nothing ever scales or
    // swings out of the frustum, which is what the old cubes did.
    enum { ARC = NPLAYERS * ARC_BARS };
    KilnTransform floor_xf, bar_xf[ARC], pad_xf[NPLAYERS];
    kiln_transform_init(&floor_xf);
    for (int i = 0; i < ARC; i++) {
        kiln_transform_init(&bar_xf[i]);
        const int p = i / ARC_BARS, k = i % ARC_BARS;
        const float a = (-55.0f + 110.0f * ((float)p * (ARC_BARS + 1) + k) / (NPLAYERS * (ARC_BARS + 1) - 2)) * 0.0174533f;
        bar_xf[i].pos = (fm_vec3_t){{ -fm_sinf(a) * 80.0f, 0, fm_cosf(a) * 80.0f - 20.0f }};
        bar_xf[i].rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
        bar_xf[i].rot_angle = a;
    }
    for (int p = 0; p < NPLAYERS; p++) {
        kiln_transform_init(&pad_xf[p]);
        const float a = (-55.0f + 110.0f * ((float)p * (ARC_BARS + 1) + 2.5f) / (NPLAYERS * (ARC_BARS + 1) - 2)) * 0.0174533f;
        pad_xf[p].pos = (fm_vec3_t){{ -fm_sinf(a) * 80.0f, 0, fm_cosf(a) * 80.0f - 20.0f }};
        pad_xf[p].rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
        pad_xf[p].rot_angle = a;
    }
    scene.cam_pos = (fm_vec3_t){{ 0, 70, -95 }};
    scene.cam_target = (fm_vec3_t){{ 0, 6, 40 }};
    kiln_scene_update(&scene);

    if (KILN_JUMP == JUMP_PATCH) {
        g_intro_active = 0;
        g_menu_open = 1;
        kiln_input_play(1, &PATCH_TAPE);
    }

    uint32_t last_ticks = get_ticks(), frame = 0;
    float intro_ms = 0.0f;

    for (;;) {
        const uint32_t ticks = get_ticks();
        const float dt_ms = clampf((float)TICKS_DISTANCE(last_ticks, ticks) * 1000.0f / (float)TICKS_PER_SECOND, 0.0f, 100.0f);
        last_ticks = ticks;
        frame++;

        kiln_input_update();

        if (g_intro_active) {
            intro_ms += dt_ms;
            int skip = 0;
            for (int p = 1; p <= NPLAYERS; p++) skip |= kiln_input_get(p)->edges != 0;
            if (skip || intro_ms >= INTRO_TOTAL_MS) {
                release_all();
                g_intro_active = 0;
                // Armed only now: armed at boot, it would count the intro as
                // idle and its first press would skip the intro.
                kiln_input_set_attract(1, &BASSLINE, 240);
            } else {
                intro_step(intro_ms);
            }
        } else if (g_menu_open) {
            handle_menu_input(frame);
        } else {
            handle_play_input();
            if (kiln_input_get(1)->edges & KILN_BTN_START) {
                release_all();
                g_menu_open = 1;
            }
        }

        update_voices(dt_ms);

        int active = 0;
        for (int i = 0; i < BASS_NOTES; i++) active += g_notes[i].active;

        // ── 3D ──────────────────────────────────────────────────────────
        kiln_frame_begin();
        kiln_scene_begin(&scene);
        kiln_transform_push(&floor_xf); kiln_prim_draw(&floor_prim); kiln_transform_pop();
        for (int p = 0; p < NPLAYERS; p++) {
            kiln_transform_push(&pad_xf[p]); kiln_prim_draw(&pad[p]); kiln_transform_pop();
        }
        for (int i = 0; i < ARC; i++) {
            const int p = i / ARC_BARS, k = i % ARC_BARS;
            const float lvl = bar_level(p, k);
            bar_xf[i].scale = (fm_vec3_t){{ 1.0f, 0.15f + 2.2f * lvl, 1.0f }};
            kiln_transform_push(&bar_xf[i]);
            kiln_prim_draw(lvl > 0.02f ? &bar_lit[p] : &bar_dim[p]);
            kiln_transform_pop();
        }

        // ── 2D ──────────────────────────────────────────────────────────
        kiln_gui_begin();
        draw_status(active);
        if (!g_intro_active && !g_menu_open) draw_scope();
        if (g_intro_active) {
            kiln_gui_panel(40, 30, 240, 44, PANEL, TEAL);
            kiln_gui_text(48, 44, TEAL, "KILN BASS SYNTH");
            kiln_gui_text(48, 56, INK, "4 pads, 4 engines, 12 notes each");
            kiln_gui_text(48, 68, DIM, "intro %d.%ds  any button skips", (int)(intro_ms / 1000.0f),
                          (int)(intro_ms / 100.0f) % 10);
            kiln_gui_bar(40, 76, 240, 4, intro_ms / INTRO_TOTAL_MS, TEAL, WELL);
        }
        // A save or load result, shown for three seconds after it lands.
        if (!g_menu_open && frame - g_store_shown_frame < 180 && g_store_shown_frame) {
            char line[48];
            store_line(line, sizeof line);
            kiln_gui_panel(80, 56, 160, 16, PANEL, g_store_status == KILN_STORE_OK ? TEAL : BAD);
            kiln_gui_text(86, 68, g_store_status == KILN_STORE_OK ? TEAL : BAD, "%s", line);
        }
        if (kiln_input_scripted(1) && !g_menu_open) {
            kiln_gui_panel(SCREEN_W - 58, 56, 50, 16, RGBA32(0xC0, 0x30, 0x60, 0xFF), RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
            kiln_gui_text(SCREEN_W - 49, 68, RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "DEMO");
        }
        // The editor covers the player columns rather than sliding under them:
        // it is 6 rows plus the save line, and the columns start at y 162.
        if (g_menu_open) draw_menu(frame);
        else draw_players();
        kiln_gui_end();
        kiln_frame_end();

        kiln_audio_update();
    }
}
