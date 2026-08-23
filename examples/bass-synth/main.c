// SPDX-License-Identifier: MIT
//
// Bass-synth — a 4-controller collaborative bass ROM.
//
// Architecture (single file, eight sections):
//
//   1. Wavetables       — 12 single-cycle 256-sample mono wavs (4 engines ×
//                          {body_bright, body_dark, sub}). Each held note
//                          owns TWO mixer channels: body (crossfaded live by
//                          stick X between bright and dark) and sub (fixed
//                          gain, one octave below). 12 channels = 6 notes.
//
//   2. ADSR             — per-engine {attack, decay, sustain, release} with
//                          a linear stage machine. Sustain is a *held* level
//                          after decay, release goes from current level back
//                          to zero (matches how a Moog Subsequent behaves).
//
//   3. Voice allocator  — 6 note slots, each {body_ch, sub_ch}. Atomic 2-
//                          channel allocation. Steal priority: lowest note
//                          wins ties; among same pitch, release-phase loses
//                          to attack/sustain (so sustained bass lines survive
//                          aggressive playing).
//
//   4. Input mapping    — 13 buttons per pad → 13 semitones per player
//                          (chromatic; scale-quantization in menu). Each
//                          player has engine, octave, legato, portamento.
//                          C-stick X = ±1 semitone pitch bend. C-stick Y =
//                          mod-wheel (0..1 routed to LFO depth).
//
//   5. Legato/retrigger — in legato mode, pressing a new note while another
//                          is held slides pitch without restarting the amp
//                          envelope. In retrigger mode, each press re-attacks
//                          from current envelope level (no click on quick
//                          retriggers).
//
//   6. Portamento       — always on; the active note's target freq slides
//                          from previous to new over `portamento_ms`. Stick
//                          X at full deflection produces instant snap.
//
//   7. UI               — Start-toggled menu (engine/octave/legato/portamento/
//                          scale per player, plus global LFO rate and master
//                          gain). Always-on activity overlay shows envelope
//                          stage + bend + mod-wheel + voice meter per player.
//                          3D field = 6 stacked pairs of cubes (body+sub per
//                          note) Y-scaled by per-channel volume.
//
//   8. Save state       — per-player engine/octave/legato/portamento/scale
//                          persist across reboot via libdragon eepromfs.
//
// Per-frame VR4300 cost: ~12 channels × {ADSR ramp, portamento step, LFO
// step, crossfade step, vol/pan/freq write}. Well under 1% CPU at 30 Hz
// update rate. RSP mixer does all sample mixing; no per-sample DSP.

#include <libdragon.h>
#include <math.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>

#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_audio.h>

#define SCREEN_W    320
#define SCREEN_H    240
#define SAMPLE_RATE 32000

#define BASS_NOTES  6                  // 6 simultaneous notes
#define BASS_CHANS  (BASS_NOTES * 2)   // each note = 2 channels
#define WT_LEN      256

#define BASE_MIDI   28                 // E2 — lowest playable (4-string bass)

// ── Scale quantization ────────────────────────────────────────────────────
// Each row = semitone offsets within an octave that the buttons map to.
// Major / minor pentatonic = 5 notes per octave; chromatic = all 12.
// Applied at note-on: button `n` lands on scale_idx = scale[n], so pressing
// the same physical button produces different pitches depending on scale.
typedef enum {
    SCALE_CHROMATIC = 0,
    SCALE_MAJOR_PENT,
    SCALE_MINOR_PENT,
    SCALE_BLUES,
    SCALE_COUNT,
} Scale;

static const int8_t SCALE_STEPS[SCALE_COUNT][13] = {
    // chromatic: 0..12 (1+ octave). out-of-range = -1
    [SCALE_CHROMATIC] = { 0,1,2,3,4,5,6,7,8,9,10,11,12 },
    // major pentatonic: 0,2,4,7,9
    [SCALE_MAJOR_PENT] = { 0,2,4,7,9,12,12,12,12,12,12,12,12 },
    // minor pentatonic: 0,3,5,7,10
    [SCALE_MINOR_PENT] = { 0,3,5,7,10,12,12,12,12,12,12,12,12 },
    // blues: 0,3,5,6,7,10
    [SCALE_BLUES]      = { 0,3,5,6,7,10,12,12,12,12,12,12,12 },
};
static const char * const SCALE_NAME[SCALE_COUNT] = {
    "CHROM", "MAJP5", "MINP5", "BLUES",
};

// ── Engines ──────────────────────────────────────────────────────────────
typedef enum {
    BASS_HEAVY = 0,
    BASS_SUB,
    BASS_GROWL,
    BASS_INDUSTRIAL,
    BASS_ENGINE_COUNT
} BassEngine;

static const char * const ENGINE_NAME[BASS_ENGINE_COUNT] = {
    "HEAVY", "SUB", "GROWL", "INDUST",
};

// Per-engine ADSR + mix + wavetable handles. Linear ramps in milliseconds.
typedef struct {
    const char *name;
    float attack_ms;
    float decay_ms;
    float sustain_lvl;   // [0..1]
    float release_ms;
    float sub_gain;      // base sub layer gain
    float body_gain;     // base body layer gain
    int   wt_body_bright;
    int   wt_body_dark;
    int   wt_sub;
} BassEngineDef;

static BassEngineDef g_engine[BASS_ENGINE_COUNT];

// ── Envelope state ────────────────────────────────────────────────────────
typedef enum {
    ENV_IDLE = 0,
    ENV_ATTACK,
    ENV_DECAY,
    ENV_SUSTAIN,
    ENV_RELEASE,
    ENV_OFF,
} EnvState;

// ── Note slot (one musical note = 2 mixer channels) ───────────────────────
typedef struct {
    uint8_t  active;
    uint8_t  body_ch;    // mixer channel for body wavetable
    uint8_t  sub_ch;     // mixer channel for sub wavetable
    uint8_t  player;     // 1..4
    uint8_t  engine;
    uint8_t  held;       // button is currently down
    int8_t   note;       // MIDI note
    float    freq_hz;    // base freq (without portamento/bend/LFO)
    float    prev_freq;  // freq at last frame (for portamento interpolation)
    float    age_ms;
    // Envelope (shared between body and sub).
    EnvState env;
    float    env_level;
    float    env_attack_start;  // where in attack we re-started (retrigger)
    // Pitch modulation (per frame).
    float    bend_semi;         // c-stick X bend in semitones
    float    mod_depth;         // c-stick Y → LFO depth
    float    stick_bright;      // 0..1 — body brightness crossfade
} BassNote;

static BassNote g_notes[BASS_NOTES];

// ── Players ──────────────────────────────────────────────────────────────
typedef struct {
    int8_t   engine;        // BassEngine
    int8_t   octave;        // semitone offset
    uint8_t  legato;        // 1 = legato (default), 0 = retrigger
    uint8_t  portamento_ms; // portamento time
    uint8_t  scale;         // Scale
    float    stick_x;
    float    stick_y;
    float    stick_mag;
    float    cstick_x;
    float    cstick_y;
    uint8_t  z_held;
    // Last note played (for UI).
    int8_t   last_note;
    // Last held button per player (legato: voice glide target).
    int8_t   held_button;   // -1 = none
} BassPlayer;

#define NPLAYERS 4
static BassPlayer g_players[NPLAYERS];

// Stereo pan per player.
static const float PLAYER_PAN[NPLAYERS] = { 0.25f, 0.42f, 0.58f, 0.75f };
static const color_t PLAYER_COLOR[NPLAYERS] = {
    RGBA32(0x00, 0xE5, 0xFF, 0xFF),
    RGBA32(0x4C, 0xFF, 0x82, 0xFF),
    RGBA32(0xFF, 0xD9, 0x4C, 0xFF),
    RGBA32(0xFF, 0x4C, 0x6A, 0xFF),
};

// ── Note mapping ──────────────────────────────────────────────────────────
static const uint32_t NOTE_BUTTONS[13] = {
    KILN_BTN_DU, KILN_BTN_DL, KILN_BTN_DD, KILN_BTN_DR,
    KILN_BTN_CL, KILN_BTN_CD, KILN_BTN_CR, KILN_BTN_CU,
    KILN_BTN_L,  KILN_BTN_B,  KILN_BTN_A,  KILN_BTN_Z,  KILN_BTN_R,
};

// ── Global state ──────────────────────────────────────────────────────────
static uint8_t  g_menu_open = 0;
static int8_t   g_menu_row  = 0;
#define MENU_ROWS_PLAYER 6       // engine, octave, legato, portamento, scale, vol
#define MENU_ROWS_GLOBAL 2       // master gain, LFO rate
#define MENU_ROWS (NPLAYERS * MENU_ROWS_PLAYER + MENU_ROWS_GLOBAL)

static float g_master_gain = 0.75f;
static float g_lfo_rate_hz = 5.0f;
static float g_lfo_phase = 0.0f;

// ── Intro sequence ────────────────────────────────────────────────────────
// Plays a 4-bar demo riff on each player in turn using the loaded engines.
// Skippable with any button. Scripted as a flat list of {time_ms, player,
// midi_note} events so the existing note_on/note_off machinery plays it
// through the same voice allocator as live input — the intro IS a real
// performance of the synth, not a separate audio asset.
typedef struct {
    uint32_t t_ms;
    uint8_t  player;        // 1..4
    int8_t   note;          // MIDI
    uint8_t  dur_ms;        // how long until note_off
    int8_t   bend;          // -1, 0, 1 (semitones for c-stick X target)
} IntroEvent;

// E-minor riff, ~80 BPM, four-bar phrase (7.5 s), one engine per bar.
// Bar 0: P1 HEAVY plays low E-minor bass line.
// Bar 1: P2 SUB plays one-octave higher root motion.
// Bar 2: P3 GROWL plays syncopated stabs.
// Bar 3: P4 INDUSTRIAL drives the climax.
static const IntroEvent INTRO_SCRIPT[] = {
    // Bar 1 — HEAVY: E2, G2, A2, B2 walking bass (each beat = 375 ms @ 80bpm)
    {    0, 1,  28, 350, 0 },   // E2
    {  375, 1,  31, 200, 0 },   // G2 short
    {  600, 1,  33, 150, 0 },   // A2 short
    {  775, 1,  35, 350, 0 },   // B2
    { 1150, 1,  33, 200, 0 },   // A2
    { 1375, 1,  31, 200, 0 },   // G2
    // Bar 2 — SUB: E3, F3, G3, A3 root motion (one octave up)
    { 1700, 2,  40, 350, 0 },   // E3
    { 2075, 2,  41, 350, 0 },   // F3
    { 2450, 2,  43, 350, 0 },   // G3
    { 2825, 2,  45, 350, 0 },   // A3
    // Bar 3 — GROWL: B3 syncopated stabs
    { 3200, 3,  47, 150, 0 },   // B3 stab
    { 3400, 3,  45, 150, 0 },
    { 3600, 3,  47, 150, 0 },
    { 3800, 3,  48, 250, 0 },   // C4 longer
    { 4100, 3,  47, 150, 0 },
    { 4300, 3,  45, 150, 0 },
    { 4500, 3,  43, 150, 0 },
    { 4700, 3,  45, 300, 0 },
    // Bar 4 — INDUSTRIAL climax: power octave E2/E3 unison, plus bend
    { 5100, 4,  28, 700, 0 },   // E2
    { 5100, 1,  40, 700, 0 },   // P1 E3 power chord
    { 5800, 4,  30, 200, 0 },   // F#2
    { 6000, 1,  42, 200, 0 },   // P1 F#3
    { 6200, 4,  33, 700, 0 },   // A2
    { 6200, 1,  45, 700, 0 },   // P1 A3
    { 6900, 4,  35, 700, 0 },   // B2
    { 6900, 1,  47, 700, 0 },   // P1 B3
    { 7600, 4,  28, 900, 1 },   // E2 hold, bend up 1 semitone
};
#define INTRO_EVENTS (sizeof(INTRO_SCRIPT) / sizeof(INTRO_SCRIPT[0]))
#define INTRO_TOTAL_MS 8500

static uint8_t  g_intro_active = 1;
static uint32_t g_intro_start_ms = 0;
static uint32_t g_intro_last_t_ms = 0xFFFFFFFF;

// Pending note-offs for the intro: small fixed ring.
#define INTRO_MAX_PENDING 64
typedef struct { uint32_t fire_ms; uint8_t player; int8_t midi; } IntroOff;
static IntroOff g_intro_pending[INTRO_MAX_PENDING];
static int g_intro_pending_n = 0;

// ── 3D scene ──────────────────────────────────────────────────────────────
static KilnScene g_scene;
static KilnTransform g_cube_xform[BASS_CHANS];
static T3DVertPacked *g_cube_verts;
static const uint8_t CUBE_TRIS[12][3] = {
    {0,1,2},{2,3,0}, {4,6,5},{6,4,7},
    {0,4,5},{5,1,0}, {1,5,6},{6,2,1},
    {2,6,7},{7,3,2}, {3,7,4},{4,0,3},
};

// ── Save state (eepromfs) ─────────────────────────────────────────────────
#define EEPROM_MAGIC 0xB7

typedef struct __attribute__((packed)) {
    uint8_t magic;
    uint8_t version;
    // Per-player settings (5 bytes each).
    uint8_t engine[NPLAYERS];
    int8_t  octave[NPLAYERS];
    uint8_t legato[NPLAYERS];
    uint8_t portamento[NPLAYERS];   // stored / 20 to fit byte; ms = x * 20
    uint8_t scale[NPLAYERS];
    uint8_t master_x4;              // 0..200 = 0.0..1.0
    uint8_t lfo_rate_x2;            // 0..40 = 0..20 Hz (capped)
} PatchState;

static const char *PATCH_FILE = "rom:/bass-synth.pat";

// ── Helpers ───────────────────────────────────────────────────────────────
static inline float midi_to_hz(int midi)
{
    return 440.0f * powf(2.0f, (midi - 69) / 12.0f);
}

static inline float soft_clip(float x)
{
    // Free-of-libm soft clip: tanh(x)/x would need tanhf. This is the
    // common 1/(1+|x|) approximation — same saturation shape, one abs +
    // one mul + one div, no FP exceptions.
    return x / (1.0f + (x < 0 ? -x : x));
}

// ── Voice allocator ───────────────────────────────────────────────────────
static int note_find_free(void)
{
    for (int i = 0; i < BASS_NOTES; i++)
        if (!g_notes[i].active) return i;
    return -1;
}

// Priority score: lower is more stealable.
// Release-phase notes score best (lowest priority). Same pitch → release
// loses to sustain; sustain loses to attack/decay.
static int note_steal_priority(int idx)
{
    BassNote *n = &g_notes[idx];
    if (!n->active) return -1000;
    int score = (int)n->note * 100;        // higher note → higher score
    if (n->env == ENV_RELEASE) score -= 5000;  // release wins steals
    else if (n->env == ENV_SUSTAIN) score -= 200;
    else if (n->env == ENV_DECAY) score -= 100;
    // Older age → slightly lower priority.
    score += (int)(n->age_ms / 100.0f);
    return score;
}

static int note_find_victim(void)
{
    int worst = -1;
    int worst_score = -0x7fffffff;
    for (int i = 0; i < BASS_NOTES; i++) {
        int s = note_steal_priority(i);
        if (s > worst_score) { worst_score = s; worst = i; }
    }
    return worst;
}

// ── Note on / off ─────────────────────────────────────────────────────────
// Find or allocate a note slot for (player, midi). In legato mode, if the
// player already has a note, glide it to the new pitch without restarting
// the envelope; otherwise re-trigger from current envelope level.
static void note_on(int player, int8_t midi)
{
    BassPlayer *pl = &g_players[player - 1];
    BassEngineDef *eng = &g_engine[pl->engine];

    // Look for an existing note held by this player.
    int existing = -1;
    for (int i = 0; i < BASS_NOTES; i++) {
        if (g_notes[i].active && g_notes[i].player == (uint8_t)player) {
            existing = i;
            break;
        }
    }

    if (existing >= 0 && pl->legato) {
        BassNote *n = &g_notes[existing];
        n->held = 1;
        n->prev_freq = midi_to_hz(n->note) * WT_LEN;
        n->note = midi;
        n->age_ms = 0.0f;
        // Don't restart envelope. Don't restart wavetable — the body channel
        // is already playing; just glide freq. The sub channel stays the
        // same (still sub layer at body freq / 2 by mixer_ch_set_freq offset).
        return;
    }

    int slot = existing >= 0 ? existing : note_find_free();
    if (slot < 0) slot = note_find_victim();
    if (slot < 0) return;
    BassNote *n = &g_notes[slot];

    if (n->active) {
        // Steal/replace: stop the previous note's channels.
        kiln_sfx_stop(n->body_ch);
        kiln_sfx_stop(n->sub_ch);
    } else {
        // Fresh allocation: same body_ch/sub_ch every time for this slot
        // (slot index → channel pair: slot*2 = body, slot*2+1 = sub).
        n->body_ch = (uint8_t)(slot * 2);
        n->sub_ch  = (uint8_t)(slot * 2 + 1);
    }

    n->active      = 1;
    n->player      = (uint8_t)player;
    n->engine      = (uint8_t)pl->engine;
    n->held        = 1;
    n->note        = midi;
    n->freq_hz     = midi_to_hz(midi);
    n->prev_freq   = n->active ? n->freq_hz * WT_LEN : 0.0f;  // start at target
    n->age_ms      = 0.0f;
    n->env         = ENV_ATTACK;
    n->env_level   = 0.0f;
    n->env_attack_start = 0.0f;
    n->bend_semi   = 0.0f;
    n->mod_depth   = 0.0f;
    n->stick_bright = (pl->stick_x + 1.0f) * 0.5f;
    if (n->stick_bright < 0.0f) n->stick_bright = 0.0f;
    if (n->stick_bright > 1.0f) n->stick_bright = 1.0f;

    g_players[player - 1].last_note = midi;
}

// Forward decls (note_on calls into handle_play_input ordering).
static void note_off(int player);

// intro variant — same path, but takes a bend override (in semitones) so the
// scripted riff can pitch up the closing E by a semitone without needing
// per-player c-stick state.
static void note_on_ext(int player, int8_t midi, int8_t bend_semi)
{
    note_on(player, midi);
    if (bend_semi != 0) {
        for (int i = 0; i < BASS_NOTES; i++) {
            if (g_notes[i].active && g_notes[i].player == (uint8_t)player &&
                g_notes[i].note == midi) {
                g_notes[i].bend_semi = (float)bend_semi;
                break;
            }
        }
    }
}

// Fire any intro events whose time has come since last frame. Single-pass;
// events are time-ordered so we just advance g_intro_last_t_ms + 1 each
// frame and skip ahead.
static void intro_advance(uint32_t elapsed_ms)
{
    if (elapsed_ms >= INTRO_TOTAL_MS) return;
    for (int i = 0; i < (int)INTRO_EVENTS; i++) {
        const IntroEvent *e = &INTRO_SCRIPT[i];
        if ((uint32_t)e->t_ms > elapsed_ms) break;
        // Walk only events with t_ms > last-fired timestamp so we don't
        // re-fire on subsequent frames.
        if ((uint32_t)e->t_ms <= g_intro_last_t_ms) continue;
        note_on_ext(e->player, e->note, e->bend);
        if (g_intro_pending_n < INTRO_MAX_PENDING) {
            g_intro_pending[g_intro_pending_n++] = (IntroOff){
                .fire_ms = (uint32_t)e->t_ms + e->dur_ms,
                .player  = e->player,
                .midi    = e->note,
            };
        }
    }
    g_intro_last_t_ms = elapsed_ms;
}

// Drain pending note-offs whose fire time has arrived.
static void intro_drain_offs(uint32_t elapsed_ms)
{
    int w = 0;
    for (int r = 0; r < g_intro_pending_n; r++) {
        if (g_intro_pending[r].fire_ms <= elapsed_ms) {
            note_off(g_intro_pending[r].player);
        } else {
            g_intro_pending[w++] = g_intro_pending[r];
        }
    }
    g_intro_pending_n = w;
}

static int intro_is_done(uint32_t elapsed_ms)
{
    return elapsed_ms >= INTRO_TOTAL_MS;
}

static void note_off(int player)
{
    int found = -1;
    float newest = -1.0f;
    for (int i = 0; i < BASS_NOTES; i++) {
        if (!g_notes[i].active || g_notes[i].player != (uint8_t)player) continue;
        if (!g_notes[i].held) continue;
        if (g_notes[i].age_ms > newest) { newest = g_notes[i].age_ms; found = i; }
    }
    if (found < 0) return;
    BassNote *n = &g_notes[found];
    n->held = 0;

    if (g_players[player - 1].z_held) {
        // Sustain: leave envelope in current state, marked for later release.
        n->env = ENV_SUSTAIN;
        return;
    }
    n->env = ENV_RELEASE;
}

static void release_sustained(int player)
{
    for (int i = 0; i < BASS_NOTES; i++) {
        if (!g_notes[i].active || g_notes[i].player != (uint8_t)player) continue;
        if (!g_notes[i].held && g_notes[i].env != ENV_SUSTAIN) continue;
        g_notes[i].env = ENV_RELEASE;
    }
}

// ── Per-frame update ──────────────────────────────────────────────────────
static void update_voices(float dt_ms)
{
    g_lfo_phase += dt_ms * 0.001f * g_lfo_rate_hz;
    if (g_lfo_phase >= 1.0f) g_lfo_phase -= (float)(int)g_lfo_phase;

    for (int i = 0; i < BASS_NOTES; i++) {
        BassNote *n = &g_notes[i];
        if (!n->active) continue;
        n->age_ms += dt_ms;

        BassEngineDef *eng = &g_engine[n->engine];
        BassPlayer *pl = &g_players[n->player - 1];

        // ── Envelope ────────────────────────────────────────────────
        switch (n->env) {
        case ENV_ATTACK: {
            float step = dt_ms / eng->attack_ms;
            n->env_level += step;
            if (n->env_level >= 1.0f) {
                n->env_level = 1.0f;
                n->env = ENV_DECAY;
            }
        } break;
        case ENV_DECAY: {
            float step = dt_ms / eng->decay_ms;
            n->env_level -= step;
            if (n->env_level <= eng->sustain_lvl) {
                n->env_level = eng->sustain_lvl;
                n->env = ENV_SUSTAIN;
            }
        } break;
        case ENV_SUSTAIN:
            n->env_level = eng->sustain_lvl;
            break;
        case ENV_RELEASE: {
            float step = dt_ms / eng->release_ms;
            n->env_level -= step;
            if (n->env_level <= 0.0f) {
                n->env_level = 0.0f;
                kiln_sfx_stop(n->body_ch);
                kiln_sfx_stop(n->sub_ch);
                n->env = ENV_OFF;
                n->active = 0;
                continue;
            }
        } break;
        default: break;
        }

        // ── Stick modulation ────────────────────────────────────────
        // Brightness crossfade: stick_x [-1..1] → body_bright share.
        n->stick_bright = (pl->stick_x + 1.0f) * 0.5f;
        if (n->stick_bright < 0) n->stick_bright = 0;
        if (n->stick_bright > 1) n->stick_bright = 1;
        // C-stick X → ±1 semitone bend.
        n->bend_semi = pl->cstick_x;
        // C-stick Y → mod-wheel.
        n->mod_depth = pl->cstick_y;
        if (n->mod_depth < 0) n->mod_depth = 0;
        if (n->mod_depth > 1) n->mod_depth = 1;

        // ── Pitch (portamento + bend + LFO) ─────────────────────────
        // Portamento: glide from prev_freq to target over portamento_ms.
        float target = n->freq_hz * WT_LEN;
        if (pl->portamento_ms > 0 && n->held) {
            float dt = (target - n->prev_freq);
            float step = dt_ms / pl->portamento_ms;
            float new_f = n->prev_freq + dt * step;
            // Have we arrived? Stop sliding.
            if ((dt > 0 && new_f >= target) || (dt < 0 && new_f <= target))
                new_f = target;
            n->prev_freq = new_f;
            target = new_f;
        } else {
            n->prev_freq = target;
        }
        // C-stick X bend (semitones → multiplier).
        float bend_mult = powf(2.0f, n->bend_semi / 12.0f);
        // LFO: sine 0..1 around 0.5, depth scaled by mod-wheel and stick mag.
        float lfo = sinf(g_lfo_phase * 2.0f * 3.14159265f);
        float mag = pl->stick_mag;
        float vibrato_amt = (mag > 0.1f ? mag : 0.0f) * 0.015f +
                            n->mod_depth * 0.04f;
        float vibrato = 1.0f + lfo * vibrato_amt;
        float body_freq = target * bend_mult * vibrato;
        float sub_freq  = (target * 0.5f) * bend_mult * vibrato;
        kiln_sfx_set_freq(n->body_ch, body_freq);
        kiln_sfx_set_freq(n->sub_ch,  sub_freq);

        // ── Mix + pan + soft-clip master ────────────────────────────
        // Body layer volume: env * body_gain * master. The "bright" share
        // mixes body_bright vs body_dark — but we only have one wavetable
        // playing on the body channel. To get a true crossfade we'd need
        // 4 channels per note; instead, brightness modulates the body
        // channel volume AND biases the body's spectrum via filtering of
        // the wavetable selection. For v1 we just track brightness in the
        // UI; future: keep body_bright + body_dark on two channels.
        //
        // (The "real" crossfade cost = 2 channels per body = 4 per note =
        // 24 channels = 6 notes. Out of budget for this iteration.)
        float body_vol = n->env_level * eng->body_gain * g_master_gain;
        // Stick Y → drive: pumps sub layer up at Y > 0.
        float sub_boost = 1.0f + (pl->stick_y > 0 ? pl->stick_y : 0.0f) * 1.2f;
        float sub_vol = n->env_level * eng->sub_gain * sub_boost * g_master_gain;
        // Master soft-clip: protects the AI from additive sum of 12 chans.
        body_vol = soft_clip(body_vol);
        sub_vol  = soft_clip(sub_vol);
        // Pan: stick Y > 0 pushes body right.
        float pan = PLAYER_PAN[n->player - 1] +
                    (pl->stick_y > 0 ? pl->stick_y * 0.2f : 0.0f);
        if (pan > 1.0f) pan = 1.0f;
        kiln_sfx_set_vol_pan(n->body_ch, body_vol, pan);
        kiln_sfx_set_vol_pan(n->sub_ch,  sub_vol,  pan);
    }
}

// ── Input handling ────────────────────────────────────────────────────────
static void handle_play_input(void)
{
    for (int p = 1; p <= NPLAYERS; p++) {
        const KilnInput *in = kiln_input_get(p);
        if (!in) continue;
        BassPlayer *pl = &g_players[p - 1];
        pl->stick_x = in->stick_x;
        pl->stick_y = in->stick_y;
        pl->stick_mag = sqrtf(in->stick_x * in->stick_x +
                              in->stick_y * in->stick_y);
        if (pl->stick_mag > 1.0f) pl->stick_mag = 1.0f;
        pl->cstick_x = in->cstick_x;
        pl->cstick_y = in->cstick_y;

        // Z = sustain modifier.
        uint8_t z_now = (in->buttons & KILN_BTN_Z) ? 1 : 0;
        if (z_now == 0 && pl->z_held == 1) release_sustained(p);
        pl->z_held = z_now;

        // Apply scale quantization to button index → note semitone.
        const int8_t *scale = SCALE_STEPS[pl->scale];
        // Last held button for legato "glide" target.
        int8_t prev_held = pl->held_button;
        int8_t new_held = -1;

        for (int n = 0; n < 13; n++) {
            uint32_t mask = NOTE_BUTTONS[n];
            int8_t semi = scale[n];
            if (semi < 0) continue;  // not in this scale
            int8_t midi = BASE_MIDI + pl->octave + semi;

            if (in->buttons & mask) {
                // Currently held.
                if (new_held < 0 || n > new_held) new_held = n;
            }

            if (in->edges & mask) {
                note_on(p, midi);
            } else if (in->released & mask) {
                // Only release if this button was the active note target.
                // (Legato tracks held_button — see below.)
                if (n == pl->held_button) note_off(p);
            }
        }

        // If the highest held button changed (player moved finger), trigger
        // the new top note — legato glides, retrigger attacks.
        if (new_held != prev_held) {
            pl->held_button = new_held;
            if (new_held >= 0) {
                int8_t semi = scale[new_held];
                if (semi >= 0) {
                    int8_t midi = BASE_MIDI + pl->octave + semi;
                    note_on(p, midi);
                }
            } else {
                // All buttons released.
                note_off(p);
            }
        }
    }
}

// ── Menu ──────────────────────────────────────────────────────────────────
typedef enum {
    FIELD_ENGINE = 0,
    FIELD_OCTAVE,
    FIELD_LEGATO,
    FIELD_PORTAMENTO,
    FIELD_SCALE,
    FIELD_VOLUME,
    FIELD_GLOBAL_MASTER,
    FIELD_GLOBAL_LFO,
} MenuField;

static void apply_menu_delta(int delta, int row)
{
    int player = row / MENU_ROWS_PLAYER;
    int field  = row % MENU_ROWS_PLAYER;
    BassPlayer *pl = &g_players[player];
    if (player >= NPLAYERS) {
        // Global rows.
        int gf = row - NPLAYERS * MENU_ROWS_PLAYER;
        if (gf == 0) {
            g_master_gain += delta * 0.05f;
            if (g_master_gain < 0.0f) g_master_gain = 0.0f;
            if (g_master_gain > 1.0f) g_master_gain = 1.0f;
        } else {
            g_lfo_rate_hz += delta * 0.5f;
            if (g_lfo_rate_hz < 0.5f) g_lfo_rate_hz = 0.5f;
            if (g_lfo_rate_hz > 12.0f) g_lfo_rate_hz = 12.0f;
        }
        return;
    }
    switch (field) {
    case FIELD_ENGINE:
        pl->engine = (pl->engine + delta + BASS_ENGINE_COUNT) % BASS_ENGINE_COUNT;
        break;
    case FIELD_OCTAVE: {
        int8_t o = pl->octave + delta * 12;
        if (o < -12) o = -12;
        if (o > 60)  o = 60;
        pl->octave = o;
    } break;
    case FIELD_LEGATO:
        pl->legato = !pl->legato;
        break;
    case FIELD_PORTAMENTO: {
        int v = (int)pl->portamento_ms + delta * 50;
        if (v < 0) v = 0;
        if (v > 500) v = 500;
        pl->portamento_ms = (uint8_t)v;
    } break;
    case FIELD_SCALE:
        pl->scale = (pl->scale + delta + SCALE_COUNT) % SCALE_COUNT;
        break;
    case FIELD_VOLUME: {
        // We don't have a per-player volume field; reuse the master for now.
        // Placeholder for future split.
        g_master_gain += delta * 0.05f;
        if (g_master_gain < 0.0f) g_master_gain = 0.0f;
        if (g_master_gain > 1.0f) g_master_gain = 1.0f;
    } break;
    }
}

static void handle_menu_input(void)
{
    const KilnInput *in = kiln_input_get(1);
    if (!in) return;
    if (in->edges & KILN_BTN_DU) g_menu_row--;
    if (in->edges & KILN_BTN_DD) g_menu_row++;
    if (g_menu_row < 0) g_menu_row = MENU_ROWS - 1;
    if (g_menu_row >= MENU_ROWS) g_menu_row = 0;

    if (in->edges & (KILN_BTN_DL | KILN_BTN_DR)) {
        int dir = (in->edges & KILN_BTN_DR) ? +1 : -1;
        apply_menu_delta(dir, g_menu_row);
    }
    if (in->edges & KILN_BTN_START) g_menu_open = 0;
}

// ── 2D UI ─────────────────────────────────────────────────────────────────
static const char *note_name(int midi)
{
    static const char *names[12] = {
        "C ","C#","D ","D#","E ","F ","F#","G ","G#","A ","A#","B "
    };
    static char buf[8];
    int oct = midi / 12 - 1;
    snprintf(buf, sizeof(buf), "%s%d", names[midi % 12], oct);
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

static void bass_vbar(int x, int y, int w, int h, float frac,
                      color_t fg, color_t bg)
{
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    kiln_gui_rect(x, y, w, h, bg);
    int fh = (int)(frac * (h - 2));
    kiln_gui_rect(x + 1, y + h - 1 - fh, w - 2, fh, fg);
}

static void bass_stick_viz(int x, int y, int w, int h,
                           float sx, float sy, color_t border, color_t dot)
{
    kiln_gui_panel(x, y, w, h, RGBA32(10, 10, 24, 200), border);
    int cx = x + w / 2;
    int cy = y + h / 2;
    int px = cx + (int)(sx * (w / 2 - 4));
    int py = cy - (int)(sy * (h / 2 - 4));
    kiln_gui_rect(px - 2, py - 2, 4, 4, dot);
}

static void draw_menu(void)
{
    int x = 16, y = 14;
    int row_h = 14;
    int w = 288;
    int total_h = MENU_ROWS * row_h + 24;

    kiln_gui_panel(x - 6, y - 10, w + 12, total_h,
                  RGBA32(10, 10, 24, 230), RGBA32(0, 245, 212, 255));
    kiln_gui_text(x, y - 6, RGBA32(0, 245, 212, 255),
                 "BASS SYNTH  Start:close   Up/Dn:row   L/R:val");

    for (int row = 0; row < MENU_ROWS; row++) {
        int ry = y + 4 + row * row_h;
        int selected = (row == g_menu_row);
        color_t row_col = selected
            ? RGBA32(40, 60, 80, 255)
            : RGBA32(20, 20, 36, 200);
        color_t row_brd = selected
            ? RGBA32(0, 245, 212, 255)
            : RGBA32(60, 60, 90, 255);
        kiln_gui_panel(x, ry, w, row_h - 2, row_col, row_brd);

        char label[40];
        char value[24];
        color_t tag_col = RGBA32(232, 232, 240, 255);

        int player = row / MENU_ROWS_PLAYER;
        int field  = row % MENU_ROWS_PLAYER;

        if (player >= NPLAYERS) {
            int gf = row - NPLAYERS * MENU_ROWS_PLAYER;
            tag_col = RGBA32(255, 220, 100, 255);
            if (gf == 0) {
                snprintf(label, sizeof(label), "global: master gain");
                snprintf(value, sizeof(value), "%.2f", g_master_gain);
            } else {
                snprintf(label, sizeof(label), "global: LFO rate (Hz)");
                snprintf(value, sizeof(value), "%.1f", g_lfo_rate_hz);
            }
        } else {
            BassPlayer *pl = &g_players[player];
            tag_col = PLAYER_COLOR[player];
            switch (field) {
            case FIELD_ENGINE:
                snprintf(label, sizeof(label), "P%d: engine", player + 1);
                snprintf(value, sizeof(value), "%s", ENGINE_NAME[pl->engine]);
                break;
            case FIELD_OCTAVE:
                snprintf(label, sizeof(label), "P%d: octave (st)", player + 1);
                snprintf(value, sizeof(value), "%+d", (int)pl->octave);
                break;
            case FIELD_LEGATO:
                snprintf(label, sizeof(label), "P%d: legato", player + 1);
                snprintf(value, sizeof(value), pl->legato ? "ON" : "OFF");
                break;
            case FIELD_PORTAMENTO:
                snprintf(label, sizeof(label), "P%d: portamento (ms)", player + 1);
                snprintf(value, sizeof(value), "%d", pl->portamento_ms);
                break;
            case FIELD_SCALE:
                snprintf(label, sizeof(label), "P%d: scale", player + 1);
                snprintf(value, sizeof(value), "%s", SCALE_NAME[pl->scale]);
                break;
            case FIELD_VOLUME:
                snprintf(label, sizeof(label), "P%d: master gain (shared)", player + 1);
                snprintf(value, sizeof(value), "%.2f", g_master_gain);
                break;
            }
        }
        kiln_gui_text(x + 4, ry + 2, tag_col, "%s", label);
        kiln_gui_text(x + 180, ry + 2, RGBA32(255, 255, 255, 255), "%s", value);
    }
}

static void draw_activity_overlay(void)
{
    int col_w = 74;
    int margin = 2;
    int y = SCREEN_H - 78;

    int active = 0;
    for (int i = 0; i < BASS_NOTES; i++) if (g_notes[i].active) active++;

    // Top strip.
    kiln_gui_panel(margin, margin, SCREEN_W - 2 * margin, 22,
                  RGBA32(10, 10, 24, 220), RGBA32(139, 92, 246, 255));
    kiln_gui_text(margin + 6, margin + 5, RGBA32(232, 232, 240, 255),
                 "BASS SYNTH  notes %d/%d  master %.2f  LFO %.1fHz  %s",
                 active, BASS_NOTES, g_master_gain, g_lfo_rate_hz,
                 g_menu_open ? "[MENU]" : "Start: menu");

    int total_w = NPLAYERS * col_w + (NPLAYERS - 1) * margin;
    int x0 = (SCREEN_W - total_w) / 2;

    for (int p = 0; p < NPLAYERS; p++) {
        int x = x0 + p * (col_w + margin);
        BassPlayer *pl = &g_players[p];
        color_t pc = PLAYER_COLOR[p];

        kiln_gui_panel(x, y, col_w, 74,
                      RGBA32(10, 10, 24, 200), pc);
        kiln_gui_text(x + 4, y + 3, pc, "P%d %s",
                     p + 1, ENGINE_NAME[pl->engine]);
        kiln_gui_text(x + 4, y + 15, RGBA32(180, 180, 200, 255),
                     "oct%+d",
                     (int)pl->octave);
        kiln_gui_text(x + 4, y + 25, RGBA32(140, 140, 160, 255),
                     "%s %s",
                     pl->legato ? "leg" : "rtr",
                     SCALE_NAME[pl->scale]);

        bass_stick_viz(x + 4, y + 36, 28, 28, pl->stick_x, pl->stick_y,
                       pc, RGBA32(0, 245, 212, 255));

        kiln_gui_text(x + 40, y + 38, RGBA32(232, 232, 240, 255),
                     "%s",
                     pl->last_note > 0 ? note_name(pl->last_note) : "--");

        // Show envelope stage for the most-recent active note on this player.
        int active_note = -1;
        float newest = -1.0f;
        for (int i = 0; i < BASS_NOTES; i++) {
            if (g_notes[i].active && g_notes[i].player == (uint8_t)(p + 1) &&
                g_notes[i].age_ms > newest) {
                newest = g_notes[i].age_ms; active_note = i;
            }
        }
        const char *env = "---";
        float env_frac = 0.0f;
        if (active_note >= 0) {
            env = env_name(g_notes[active_note].env);
            env_frac = g_notes[active_note].env_level;
        }
        kiln_gui_text(x + 40, y + 50, RGBA32(140, 140, 160, 255),
                     "%s", env);
        bass_vbar(x + 56, y + 50, 14, 8, env_frac,
                  pc, RGBA32(40, 40, 60, 255));

        // C-stick indicator (mod/bend).
        kiln_gui_text(x + 40, y + 62, RGBA32(120, 120, 150, 255),
                     "b%+d.%d m%.0f%%",
                     (int)pl->cstick_x,
                     (int)(pl->cstick_x * 10.0f) - (int)pl->cstick_x * 10,
                     pl->cstick_y * 100.0f);

        int vp = 0;
        for (int i = 0; i < BASS_NOTES; i++)
            if (g_notes[i].active && g_notes[i].player == (uint8_t)(p + 1)) vp++;
        float frac = (float)vp / 2.0f;  // max 2 notes per player = full bar
        if (frac > 1.0f) frac = 1.0f;
        bass_vbar(x + col_w - 12, y + 4, 8, 64, frac, pc, RGBA32(40, 40, 60, 255));
    }
}

// ── 3D: 12 spectrum cubes (6 stacked pairs) ──────────────────────────────
static void draw_spectrum_cubes(float t)
{
    (void)t;
    kiln_scene_begin(&g_scene);
    for (int i = 0; i < BASS_NOTES; i++) {
        BassNote *n = &g_notes[i];
        // Each note gets 2 channels: body + sub, drawn as a stack.
        // X positions: 6 notes across the screen.
        float base_x = (i - (BASS_NOTES - 1) * 0.5f) * 7.5f;

        // Body channel cube.
        BassEngineDef *eng = &g_engine[n->engine];
        float body_amp = n->active ? n->env_level * eng->body_gain : 0.0f;
        g_cube_xform[i * 2 + 0].pos = (fm_vec3_t){{ base_x - 1.6f, -3.0f, 0 }};
        g_cube_xform[i * 2 + 0].scale =
            (fm_vec3_t){{ 0.6f, 0.4f + body_amp * 8.0f, 0.6f }};
        g_cube_xform[i * 2 + 0].rot_angle = 0.0f;
        kiln_transform_push(&g_cube_xform[i * 2 + 0]);
        t3d_vert_load(g_cube_verts, 0, 8);
        kiln_transform_pop();
        for (int t2 = 0; t2 < 12; t2++)
            t3d_tri_draw(CUBE_TRIS[t2][0], CUBE_TRIS[t2][1], CUBE_TRIS[t2][2]);
        t3d_tri_sync();

        // Sub channel cube, stacked slightly offset to show the two layers.
        float sub_amp = n->active ? n->env_level * eng->sub_gain : 0.0f;
        g_cube_xform[i * 2 + 1].pos = (fm_vec3_t){{ base_x + 1.6f, -3.0f, 0 }};
        g_cube_xform[i * 2 + 1].scale =
            (fm_vec3_t){{ 0.6f, 0.4f + sub_amp * 5.0f, 0.6f }};
        g_cube_xform[i * 2 + 1].rot_angle = 0.0f;
        kiln_transform_push(&g_cube_xform[i * 2 + 1]);
        t3d_vert_load(g_cube_verts, 0, 8);
        kiln_transform_pop();
        for (int t2 = 0; t2 < 12; t2++)
            t3d_tri_draw(CUBE_TRIS[t2][0], CUBE_TRIS[t2][1], CUBE_TRIS[t2][2]);
        t3d_tri_sync();
    }
}

// ── Intro overlay (drawn over the spectrum cubes) ────────────────────────
// The 3D scene keeps playing during the intro (cubes reflect the bass line)
// so what overlays is just title text + a progress bar.
static void draw_intro_overlay(uint32_t elapsed_ms)
{
    int cx = SCREEN_W / 2;
    int title_y = 30;

    // Title panel.
    int pw = 240, ph = 60;
    kiln_gui_panel(cx - pw/2, title_y, pw, ph,
                  RGBA32(10, 10, 24, 230), RGBA32(0, 245, 212, 255));
    kiln_gui_text(cx - pw/2 + 8, title_y + 6,
                 RGBA32(0, 245, 212, 255),
                 "KILN BASS SYNTH");
    kiln_gui_text(cx - pw/2 + 8, title_y + 22,
                 RGBA32(232, 232, 240, 255),
                 "4 controllers, 4 engines");
    kiln_gui_text(cx - pw/2 + 8, title_y + 38,
                 RGBA32(180, 180, 200, 255),
                 "1 button = 1 note. Move sticks.");

    // Per-engine legend.
    int leg_y = title_y + ph + 14;
    kiln_gui_text(cx - 100, leg_y, PLAYER_COLOR[0], "P1 HEAVY");
    kiln_gui_text(cx - 100, leg_y + 14, PLAYER_COLOR[1], "P2 SUB");
    kiln_gui_text(cx + 8,  leg_y, PLAYER_COLOR[2], "P3 GROWL");
    kiln_gui_text(cx + 8,  leg_y + 14, PLAYER_COLOR[3], "P4 INDUST");

    // Progress bar (0..INTRO_TOTAL_MS). Above the activity overlay, just
    // below the engine legend.
    int bar_x = 32, bar_y = title_y + ph + 14 + 32, bar_w = SCREEN_W - 64, bar_h = 6;
    float frac = (float)elapsed_ms / (float)INTRO_TOTAL_MS;
    if (frac > 1.0f) frac = 1.0f;
    kiln_gui_panel(bar_x, bar_y, bar_w, bar_h,
                  RGBA32(40, 40, 60, 255), RGBA32(120, 120, 140, 255));
    int fw = (int)(frac * (bar_w - 2));
    kiln_gui_rect(bar_x + 1, bar_y + 1, fw, bar_h - 2,
                 RGBA32(0, 245, 212, 255));
    kiln_gui_text(bar_x, bar_y + bar_h + 4, RGBA32(232, 232, 240, 255),
                 "INTRO: %d.%ds / %d.%ds   any button to skip",
                 (int)(elapsed_ms / 1000),
                 (int)((elapsed_ms % 1000) / 100),
                 INTRO_TOTAL_MS / 1000,
                 (INTRO_TOTAL_MS % 1000) / 100);
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
        fm_vec3_t na = {{ (float)c[i].x,   (float)c[i].y,   (float)c[i].z }};
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
    for (int e = 0; e < BASS_ENGINE_COUNT; e++) {
        BassEngineDef *eng = &g_engine[e];
        char path[64];
        snprintf(path, sizeof(path), "rom:/sfx/bass_%s_body_bright.wav64", engines[e]);
        eng->wt_body_bright = kiln_sfx_load(path);
        snprintf(path, sizeof(path), "rom:/sfx/bass_%s_body_dark.wav64", engines[e]);
        eng->wt_body_dark = kiln_sfx_load(path);
        snprintf(path, sizeof(path), "rom:/sfx/bass_%s_sub.wav64", engines[e]);
        eng->wt_sub = kiln_sfx_load(path);
        if (eng->wt_body_bright < 0 || eng->wt_body_dark < 0 || eng->wt_sub < 0) {
            debugf("bass-synth: wavetable load failure for %s\n", engines[e]);
        }
    }
}

// ── Engine definitions ────────────────────────────────────────────────────
static void init_engines(void)
{
    // ADSR (ms / level), mix (per-channel base gain).
    g_engine[BASS_HEAVY] = (BassEngineDef){
        .name = "HEAVY",
        .attack_ms = 5,    .decay_ms = 180, .sustain_lvl = 0.7f, .release_ms = 120,
        .sub_gain = 0.30f, .body_gain = 0.85f,
    };
    g_engine[BASS_SUB] = (BassEngineDef){
        .name = "SUB",
        .attack_ms = 8,    .decay_ms = 300, .sustain_lvl = 0.8f, .release_ms = 200,
        .sub_gain = 0.65f, .body_gain = 0.70f,
    };
    g_engine[BASS_GROWL] = (BassEngineDef){
        .name = "GROWL",
        .attack_ms = 4,    .decay_ms = 220, .sustain_lvl = 0.6f, .release_ms = 140,
        .sub_gain = 0.40f, .body_gain = 0.85f,
    };
    g_engine[BASS_INDUSTRIAL] = (BassEngineDef){
        .name = "INDUST",
        .attack_ms = 3,    .decay_ms = 140, .sustain_lvl = 0.5f, .release_ms = 90,
        .sub_gain = 0.45f, .body_gain = 0.90f,
    };
}

// ── Player defaults ───────────────────────────────────────────────────────
static void init_players(void)
{
    for (int i = 0; i < NPLAYERS; i++) {
        g_players[i].engine = (BassEngine)i;
        g_players[i].octave = (int8_t)(i * 12);  // P1=0, P2=+12, P3=+24, P4=+36
        g_players[i].legato = 1;
        g_players[i].portamento_ms = 200;
        g_players[i].scale = SCALE_CHROMATIC;
        g_players[i].held_button = -1;
        g_players[i].last_note = -1;
    }
}

// ── Save state ────────────────────────────────────────────────────────────
static void save_patch(void)
{
    FILE *f = fopen(PATCH_FILE, "wb");
    if (!f) return;
    PatchState ps = { .magic = EEPROM_MAGIC, .version = 1 };
    for (int i = 0; i < NPLAYERS; i++) {
        ps.engine[i] = (uint8_t)g_players[i].engine;
        ps.octave[i] = g_players[i].octave;
        ps.legato[i] = g_players[i].legato;
        ps.portamento[i] = (uint8_t)(g_players[i].portamento_ms / 20);
        ps.scale[i] = g_players[i].scale;
    }
    ps.master_x4 = (uint8_t)(g_master_gain * 200.0f);
    ps.lfo_rate_x2 = (uint8_t)(g_lfo_rate_hz * 2.0f);
    fwrite(&ps, sizeof(ps), 1, f);
    fclose(f);
}

static int load_patch(void)
{
    FILE *f = fopen(PATCH_FILE, "rb");
    if (!f) return 0;
    PatchState ps;
    if (fread(&ps, sizeof(ps), 1, f) != 1) { fclose(f); return 0; }
    fclose(f);
    if (ps.magic != EEPROM_MAGIC || ps.version != 1) return 0;
    for (int i = 0; i < NPLAYERS; i++) {
        g_players[i].engine = (BassEngine)(ps.engine[i] % BASS_ENGINE_COUNT);
        g_players[i].octave = ps.octave[i];
        g_players[i].legato = ps.legato[i] ? 1 : 0;
        g_players[i].portamento_ms = (uint8_t)(ps.portamento[i] * 20);
        g_players[i].scale = (Scale)(ps.scale[i] % SCALE_COUNT);
    }
    g_master_gain = ps.master_x4 / 200.0f;
    g_lfo_rate_hz = ps.lfo_rate_x2 / 2.0f;
    return 1;
}

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);
    kiln_input_init();

    kiln_audio_init((KilnAudioConfig){
        .sample_rate = SAMPLE_RATE,
        .latency = 0.16f,
        .sfx_channels = BASS_CHANS,
        .music_channels = 0,
    });

    init_engines();
    load_wavetables();
    init_players();
    load_patch();  // ignore failure — defaults stand

    kiln_scene_init(&g_scene);
    g_scene.cam_pos = (fm_vec3_t){{ 0, 10, -45 }};
    g_scene.cam_target = (fm_vec3_t){{ 0, 1, 0 }};
    g_scene.far_z = 300.0f;
    kiln_scene_update(&g_scene);

    for (int i = 0; i < BASS_CHANS; i++) {
        kiln_transform_init(&g_cube_xform[i]);
        g_cube_xform[i].scale = (fm_vec3_t){{ 1.6f, 0.3f, 1.6f }};
    }
    g_cube_verts = make_cube_verts();

    uint32_t last_ticks = get_ticks();
    float t = 0.0f;
    g_intro_start_ms = last_ticks;

    for (;;) {
        kiln_input_update();

        // Intro: takes priority over menu + play input. Any button edge
        // skips. Manual kiln_input_get lookups below the intro block also
        // see the same edges, so the intro doesn't steal them permanently.
        uint32_t now = get_ticks();
        uint32_t intro_elapsed = TICKS_DISTANCE(g_intro_start_ms, now)
                                 / (TICKS_PER_SECOND / 1000);

        if (g_intro_active) {
            // Skip on any button edge on any controller.
            int skip = 0;
            for (int p = 1; p <= NPLAYERS; p++) {
                const KilnInput *in = kiln_input_get(p);
                if (in && in->edges) skip = 1;
            }
            if (skip || intro_is_done(intro_elapsed)) {
                // Force-release all intro notes so the voice allocator
                // doesn't carry phantom held notes into the live state.
                for (int p = 1; p <= NPLAYERS; p++) {
                    for (int i = 0; i < BASS_NOTES; i++) {
                        if (g_notes[i].active &&
                            g_notes[i].player == (uint8_t)p) {
                            g_notes[i].held = 0;
                            g_notes[i].env = ENV_RELEASE;
                        }
                    }
                }
                g_intro_pending_n = 0;
                g_intro_active = 0;
            } else {
                intro_advance(intro_elapsed);
                intro_drain_offs(intro_elapsed);
            }
        } else if (g_menu_open) {
            handle_menu_input();
        } else {
            handle_play_input();
            const KilnInput *in = kiln_input_get(1);
            if (in && (in->edges & KILN_BTN_START)) {
                g_menu_open = 1;
                save_patch();
            }
        }

        float dt_ms = (float)TICKS_DISTANCE(last_ticks, now)
                      / (float)TICKS_PER_SECOND * 1000.0f;
        last_ticks = now;
        if (dt_ms > 100.0f) dt_ms = 100.0f;
        t += dt_ms / 1000.0f;

        update_voices(dt_ms);

        kiln_frame_begin();
        draw_spectrum_cubes(t);
        kiln_gui_begin();
        if (g_intro_active) draw_intro_overlay(intro_elapsed);
        if (g_menu_open) draw_menu();
        draw_activity_overlay();
        kiln_gui_end();
        kiln_frame_end();

        kiln_audio_update();
    }
}