// SPDX-License-Identifier: MPL-2.0
//
// pm_cine.c — see pm_cine.h.

#include "pm_cine.h"

#ifdef KILN_DEBUG

#include <libdragon.h>
#include <math.h>
#include <string.h>

#include <kiln/kiln_debugdraw.h>
#include <kiln/kiln_gui.h>

#include "pm_arrival.h"
#include "pm_camkey.h"
#include "pm_cine_lint.h"
#include "pm_credits.h"
#include "pm_demo.h"
#include "pm_fx.h"
#include "pm_intake.h"
#include "pm_lab.h"
#include "pm_narration.h"
#include "pm_sfx.h"

// ── The registry ───────────────────────────────────────────────────────
#define X(NAME, PTR, LAB) { #NAME, PTR, LAB },
const PMCineEntry pm_cine_shots[] = { PM_SHOT_LIST(X) };
#undef X
const int pm_cine_shot_count =
    (int)(sizeof pm_cine_shots / sizeof pm_cine_shots[0]);

// ── Build-flag defaults ────────────────────────────────────────────────
// Each is a Makefile flag (see PetaByte-Madness/Makefile) that only means
// anything together with KILN_DEBUG, which is why they are read here rather
// than guarded at every use.
#ifndef PM_CINE
#define PM_CINE 0
#endif
#ifndef PM_SHOT_AT
#define PM_SHOT_AT (-1.0f)   /* negative = no automatic seek */
#endif
// Through a variable rather than compared as a literal: `PM_SHOT_AT >= 0.0f`
// with the default in place is a constant-false comparison, which is exactly
// the shape -Wtype-limits and friends exist to complain about.
static const float g_shot_at = (float)(PM_SHOT_AT);
#ifndef PM_SHOT_LADDER
#define PM_SHOT_LADDER 0
#endif

/** Frames each ladder rung is held for. Counted in FRAMES, not seconds, for
 *  the same reason the seek is a re-simulation: a capture must not depend on
 *  how fast the emulator happens to be running. */
#define LADDER_HOLD_FRAMES 120

/** The fixed step a seek pumps at. 1/60 is the frame rate the shots were
 *  authored against; using the live dt instead would make a seek land somewhere
 *  slightly different on every machine, which is the whole thing being fixed. */
#define SEEK_STEP (1.0f / 60.0f)

/** Guard against a runaway pump if a shot's duration is ever nonsense.
 *  60 * 300 is five minutes of shot, comfortably past NARRATION's 254 s. */
#define SEEK_MAX_STEPS 18000

// ── Transport state ────────────────────────────────────────────────────
static const float RATES[] = { 0.0f, 0.25f, 0.5f, 1.0f, 2.0f };
#define RATE_COUNT ((int)(sizeof RATES / sizeof RATES[0]))
#define RATE_NORMAL 3

static int   g_armed = PM_CINE;
static int   g_rate = RATE_NORMAL;
static int   g_paused;
static int   g_step;          /* one frame requested while paused */
static int   g_seeking;
static float g_legend_left;   /* seconds of button legend still to draw */

static const PMDemoShot *g_seen_shot;   /* to notice a shot change */
static int   g_auto_done;               /* PM_SHOT_AT already applied */
static int   g_rung = -1;
static int   g_rung_frames;

// ── The detached camera ────────────────────────────────────────────────
static int       g_detached;
static fm_vec3_t g_fly_pos;
static float     g_fly_yaw, g_fly_pitch;
static float     g_shot_look_dist = 256.0f;   /* for the pose readout */

// Base speed in world units per second. The lab is 631 units across and the
// island is 13,000, so a single fly speed is unusable at one end or the other —
// but rather than spend a button on a boost, the speed scales off the SHOT'S
// OWN far plane, which is already the best available statement of how big the
// thing being looked at is (pm_demo.h's near_z/far_z comment makes that case
// for the renderer; it holds just as well for the camera).
#define FLY_MOVE  420.0f
#define FLY_LOOK  2.4f     /* radians per second */
#define PITCH_LIM 1.5f

// ── The cue ring ───────────────────────────────────────────────────────
// Pointers, never copies: every tag in the game is a string literal (a clip
// name, an SFX path, an "fx.flash"), so the ring costs eight bytes an entry and
// no allocation — which matters on a console with 4 MB and an engine that
// deliberately never mallocs per-object.
#define CUE_CAP 32
typedef struct { float t; const char *tag; } PMCue;
static PMCue g_cues[CUE_CAP];
static int   g_cue_count;      /* total ever recorded this shot */

static void cue_reset(void) { g_cue_count = 0; }

void pm_cine_cue(const char *tag)
{
    if (!tag) return;
    g_cues[g_cue_count % CUE_CAP].t = pm_demo_elapsed();
    g_cues[g_cue_count % CUE_CAP].tag = tag;
    g_cue_count++;
}

/** The readable part of a tag. SFX cues carry their DFS path — the only string
 *  pm_sfx.c has that is guaranteed to outlive the ring and guaranteed not to
 *  drift from the sound it names — so "rom:/sfx/mri_start.wav64" is trimmed to
 *  "mri_start" for display. A tag with no '/' is printed whole.
 *
 *  Writes into `buf` because the trim needs to drop a suffix as well as a
 *  prefix, and the source is const. */
static const char *cue_short(const char *tag, char *buf, int cap)
{
    const char *s = strrchr(tag, '/');
    s = s ? s + 1 : tag;
    int i = 0;
    while (s[i] && s[i] != '.' && i < cap - 1) { buf[i] = s[i]; i++; }
    // A tag like "fx.flash" is all suffix by that rule, so keep it whole.
    if (i == 0) return tag;
    buf[i] = '\0';
    return buf;
}

// ── Seek ───────────────────────────────────────────────────────────────
int pm_cine_seeking(void) { return g_seeking; }

int pm_cine_repro(void) { return g_shot_at >= 0.0f; }

void pm_cine_seek(float t)
{
    const PMDemoShot *shot = pm_demo_current();
    if (!shot || g_seeking) return;

    if (t < 0.0f) t = 0.0f;
    // Never land ON the end. pm_demo_update sets `done` at >= duration, and
    // pm_screens' intro_update advances the screen the moment it sees that —
    // so a seek to the last frame would walk straight off the shot being
    // studied. Stopping a hair short holds the final pose instead.
    const float last = shot->duration * 0.999f;
    if (t > last) t = last;

    g_seeking = 1;
    // Muted rather than skipped: the cue TRACE should come back with the
    // shot's full history up to `t`, because "what fired before what" is one of
    // the two questions this whole module exists to answer. Only the audible
    // half is dropped.
    pm_sfx_mute(1);
    cue_reset();

    pm_demo_play(shot, pm_demo_looping());
    int steps = 0;
    while (pm_demo_elapsed() < t && steps < SEEK_MAX_STEPS) {
        pm_demo_update(SEEK_STEP);
        steps++;
    }

    // Whatever shake, flash and hit-stop the pumped cues accumulated all landed
    // in the same frame, so the state they leave behind is meaningless. Clear
    // it rather than let a seek arrive under a white flash.
    pm_fx_reset();
    pm_sfx_mute(0);
    g_seeking = 0;
}

// ── The clock ──────────────────────────────────────────────────────────
float pm_cine_dt(float dt)
{
    if (!g_armed) return dt;
    if (g_seeking) return dt;   /* the pump drives pm_demo_update directly */
    if (g_step) { g_step = 0; return SEEK_STEP; }
    if (g_paused) return 0.0f;
    return dt * RATES[g_rate];
}

// ── Input ──────────────────────────────────────────────────────────────
// L+R arms. That pair is read by nothing on any cinematic screen — the same
// "a chord the game cannot press by accident" reasoning pm_debug.c gives for
// C-Left + C-Right, and kiln_console.h for its four-button chord.
#define ARM_CHORD (KILN_BTN_L | KILN_BTN_R)

int pm_cine_grabs_input(void) { return g_armed; }

void pm_cine_input(const KilnInput *in)
{
    if (!in) return;

    if ((in->edges & ARM_CHORD) && (in->buttons & ARM_CHORD) == ARM_CHORD) {
        g_armed = !g_armed;
        g_legend_left = g_armed ? 4.0f : 0.0f;
        if (!g_armed) { g_paused = 0; g_rate = RATE_NORMAL; g_detached = 0; }
        return;   /* do not also read L and R as their own bindings */
    }
    if (!g_armed) return;

    const PMDemoShot *shot = pm_demo_current();
    const float t = pm_demo_elapsed();

    if (in->edges & KILN_BTN_START) { g_paused = !g_paused; g_legend_left = 2.0f; }
    if (in->edges & KILN_BTN_R)     { g_step = 1; g_paused = 1; }
    if (in->edges & KILN_BTN_L) {
        // Skips index 0, which is the rate 0 slot — pausing is START's job, and
        // a rate of zero reachable two ways is two states that look identical
        // and behave differently.
        g_rate = g_rate + 1 >= RATE_COUNT ? 1 : g_rate + 1;
    }
    if (in->edges & KILN_BTN_Z) {
        g_detached = !g_detached;
        if (g_detached) {
            // Start from wherever the shot currently is, looking where it
            // looks. Anything else means the first thing you see after
            // detaching is somewhere you did not ask to be.
            const KilnScene *sc = pm_demo_scene();
            if (sc) {
                g_fly_pos = sc->cam_pos;
                fm_vec3_t d;
                for (int i = 0; i < 3; i++)
                    d.v[i] = sc->cam_target.v[i] - sc->cam_pos.v[i];
                const float h = sqrtf(d.v[0] * d.v[0] + d.v[2] * d.v[2]);
                g_fly_yaw = (h > 0.0f || d.v[0] != 0.0f)
                              ? atan2f(d.v[0], d.v[2]) : 0.0f;
                g_fly_pitch = atan2f(d.v[1], h > 0.0001f ? h : 0.0001f);
                g_shot_look_dist =
                    sqrtf(d.v[0] * d.v[0] + d.v[1] * d.v[1] + d.v[2] * d.v[2]);
                if (g_shot_look_dist < 1.0f) g_shot_look_dist = 256.0f;
            }
        }
    }

    if (shot && shot->key_count > 0) {
        if (in->edges & KILN_BTN_DL) pm_cine_seek(t - 0.5f);
        if (in->edges & KILN_BTN_DR) pm_cine_seek(t + 0.5f);

        // Key stepping seeks to just AFTER the key's own time, so the frame
        // shown is the one the key is composing rather than the last frame of
        // the segment leading into it.
        if (in->edges & KILN_BTN_DU) {
            float want = 0.0f;
            for (int i = 0; i < shot->key_count; i++)
                if (shot->keys[i].t < t - 0.05f) want = shot->keys[i].t;
            pm_cine_seek(want);
        }
        if (in->edges & KILN_BTN_DD) {
            float want = shot->duration;
            for (int i = shot->key_count - 1; i >= 0; i--)
                if (shot->keys[i].t > t + 0.05f) want = shot->keys[i].t;
            pm_cine_seek(want);
        }
    }
}

// ── Free-fly ───────────────────────────────────────────────────────────
// Deliberately NOT kiln_fpscam. That camera probes the ground against the clip
// world and falls when there is none — and no cinematic screen installs one
// (main.c and pm_lab.c call kiln_clip_set_world for PLAY and LAB only). A
// camera with no gravity and no collision is both simpler and the correct
// model for the thing being built here.
//
// The look bindings take BOTH the C-stick and the C buttons. kiln_fpscam reads
// only the C-stick, which an N64 controller does not have — so on the pad this
// game actually ships for, that path is dead.
static void fly_update(const KilnInput *in, float dt)
{
    if (!in) return;

    float dyaw = in->cstick_x, dpitch = in->cstick_y;
    if (in->buttons & KILN_BTN_CL) dyaw   -= 1.0f;
    if (in->buttons & KILN_BTN_CR) dyaw   += 1.0f;
    if (in->buttons & KILN_BTN_CU) dpitch += 1.0f;
    if (in->buttons & KILN_BTN_CD) dpitch -= 1.0f;

    g_fly_yaw   += dyaw   * FLY_LOOK * dt;
    g_fly_pitch += dpitch * FLY_LOOK * dt;
    if (g_fly_pitch >  PITCH_LIM) g_fly_pitch =  PITCH_LIM;
    if (g_fly_pitch < -PITCH_LIM) g_fly_pitch = -PITCH_LIM;

    // Same basis kiln_fpscam_forward/right use, so a pose read off this camera
    // means the same thing as one read off the game's.
    const float cp = fm_cosf(g_fly_pitch);
    const float fx = fm_sinf(g_fly_yaw) * cp;
    const float fy = fm_sinf(g_fly_pitch);
    const float fz = fm_cosf(g_fly_yaw) * cp;
    const float rx = fm_cosf(g_fly_yaw);
    const float rz = -fm_sinf(g_fly_yaw);

    const KilnScene *sc = pm_demo_scene();
    float scale = sc ? sc->far_z / 1000.0f : 1.0f;
    if (scale < 0.25f) scale = 0.25f;
    if (scale > 8.0f)  scale = 8.0f;
    const float sp = FLY_MOVE * scale;
    g_fly_pos.v[0] += (fx * in->stick_y + rx * in->stick_x) * sp * dt;
    g_fly_pos.v[1] += (fy * in->stick_y) * sp * dt;
    g_fly_pos.v[2] += (fz * in->stick_y + rz * in->stick_x) * sp * dt;
    if (in->buttons & KILN_BTN_A) g_fly_pos.v[1] += sp * dt;
    if (in->buttons & KILN_BTN_B) g_fly_pos.v[1] -= sp * dt;
}

static void fly_look(fm_vec3_t *out)
{
    const float cp = fm_cosf(g_fly_pitch);
    out->v[0] = g_fly_pos.v[0] + fm_sinf(g_fly_yaw) * cp * g_shot_look_dist;
    out->v[1] = g_fly_pos.v[1] + fm_sinf(g_fly_pitch) * g_shot_look_dist;
    out->v[2] = g_fly_pos.v[2] + fm_cosf(g_fly_yaw) * cp * g_shot_look_dist;
}

int pm_cine_detached(void) { return g_armed && g_detached; }

int pm_cine_override_camera(KilnScene *scene)
{
    if (!pm_cine_detached() || !scene) return 0;
    scene->cam_pos = g_fly_pos;
    fly_look(&scene->cam_target);
    return 1;
}

// ── Per-frame bookkeeping ──────────────────────────────────────────────
void pm_cine_frame(float dt)
{
    const KilnInput *in = kiln_input_get(0);

    if (g_legend_left > 0.0f) g_legend_left -= dt;

    const PMDemoShot *shot = pm_demo_current();
    if (shot != g_seen_shot) {
        g_seen_shot = shot;
        cue_reset();
        g_auto_done = 0;
        g_rung = -1;
        g_rung_frames = 0;
    }
    if (!shot) return;

    // ── PM_SHOT_AT: one exact frame, reproducibly ──────────────────────
    // This is what replaces `./dev shot <rom> out.png 6`. Six seconds of WALL
    // CLOCK is a guess about emulator speed and machine load; a seek to 6.0 s
    // of SHOT time is the same picture every run, on any machine.
    if (!g_auto_done && g_shot_at >= 0.0f) {
        g_auto_done = 1;
        g_armed = 1;
        pm_cine_seek(g_shot_at);
        g_paused = 1;
    }

    // ── PM_SHOT_LADDER: the whole shot from one boot ───────────────────
    // Rungs sit at the MIDPOINT of each slice rather than at its edges: t = 0
    // is before anything has happened and t = duration is after the shot is
    // over, and neither is a frame worth photographing.
    if (PM_SHOT_LADDER > 0 && g_rung < PM_SHOT_LADDER) {
        g_armed = 1;
        if (g_rung < 0 || ++g_rung_frames >= LADDER_HOLD_FRAMES) {
            g_rung = (g_rung < 0) ? 0 : g_rung + 1;
            g_rung_frames = 0;
            if (g_rung < PM_SHOT_LADDER) {
                const float f = ((float)g_rung + 0.5f) / (float)PM_SHOT_LADDER;
                pm_cine_seek(shot->duration * f);
                g_paused = 1;
            }
            // Past the last rung the loop stops re-entering entirely and the
            // shot stays parked on it, so a recording that overruns still ends
            // on a labelled frame rather than drifting somewhere unlabelled.
        }
    }

    if (pm_cine_detached()) fly_update(in, dt);
}

// ── The spatial half ───────────────────────────────────────────────────
void pm_cine_draw3d(const KilnScene *sc, int w, int h)
{
    if (!pm_cine_detached() || !sc) return;
    const PMDemoShot *shot = pm_demo_current();
    if (!shot || shot->key_count == 0) return;

    kiln_dd_begin(sc, w, h);

    // Where the SHOT's camera is right now — not where the viewer is. Drawn
    // only when detached, because undetached the two are the same point and a
    // frustum drawn from inside itself is a full-screen X.
    fm_vec3_t eye, look;
    pm_camkey_sample(shot->keys, shot->key_count, pm_demo_looping(),
                     pm_demo_elapsed(), &eye, &look);

    kiln_dd_axes(eye, 24.0f);
    kiln_dd_line(eye, look, RGBA32(255, 230, 120, 200));
    kiln_dd_point(look, 3, RGBA32(255, 230, 120, 255));
    kiln_dd_text(eye, RGBA32(255, 230, 120, 255), "shot %.2f",
                pm_demo_elapsed());

    // The frustum the shot is actually rendering with. scene->near_z/far_z are
    // what pm_demo_apply_frustum resolved for THIS shot, so a far plane cutting
    // the thing the shot is pointed at is now a picture rather than two numbers
    // in a text panel.
    kiln_dd_frustum(eye, look, sc->fov_deg, 4.0f / 3.0f,
                   sc->near_z, sc->far_z, RGBA32(120, 200, 255, 110));

    kiln_dd_end();
}

// ── The overlay ────────────────────────────────────────────────────────
static int tmap(float t, float dur, int x0, int x1)
{
    if (dur <= 0.0f) return x0;
    float f = t / dur;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return x0 + (int)((float)(x1 - x0) * f);
}

void pm_cine_draw(int w, int h)
{
    if (!g_armed) return;

    const color_t bg   = RGBA32(0, 0, 0, 190);
    const color_t bord = RGBA32(60, 70, 90, 200);
    const color_t key  = RGBA32(120, 200, 255, 255);
    const color_t val  = RGBA32(235, 235, 235, 255);
    const color_t amber = RGBA32(255, 200, 80, 255);

    const PMDemoShot *shot = pm_demo_current();
    const float t = pm_demo_elapsed();
    const float dur = shot ? shot->duration : 0.0f;

    const int y0 = h - 34;
    kiln_gui_panel(2, y0, w - 4, 32, bg, bord);

    // Which key is live, and how far through its segment — the two numbers that
    // say "the camera is between k4 and k5" without reading the table.
    int ki = 0;
    float f = 0.0f;
    if (shot && shot->key_count > 0) {
        const PMCamKey *k = shot->keys;
        const int n = shot->key_count;
        while (ki < n - 2 && t >= k[ki + 1].t) ki++;
        const int i2 = (ki + 1 < n) ? ki + 1 : ki;
        const float span = k[i2].t - k[ki].t;
        f = span > 0.0f ? (t - k[ki].t) / span : 0.0f;
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
    }

    kiln_gui_text(6, y0 + 9, key,
                 "%s  t %6.2f of %6.2f  x%.2f%s  k%d of %d  f %.2f",
                 shot ? shot->name : "no shot", t, dur, RATES[g_rate],
                 g_paused ? " PAUSE" : "", ki,
                 shot ? shot->key_count : 0, f);

    // ── The bar ────────────────────────────────────────────────────────
    const int bx0 = 6, bx1 = w - 7, by = y0 + 15;
    kiln_gui_rect(bx0, by, bx1 - bx0, 4, RGBA32(40, 48, 60, 220));

    if (shot && shot->key_count > 0)
        for (int i = 0; i < shot->key_count; i++) {
            const int x = tmap(shot->keys[i].t, dur, bx0, bx1);
            kiln_gui_rect(x, by - 3, 1, 10, RGBA32(200, 240, 255, 220));
        }

    // Cue ticks under the bar. Only the CUE_CAP most recent survive the ring,
    // which for every shot in this game is all of them.
    const int shown = g_cue_count < CUE_CAP ? g_cue_count : CUE_CAP;
    for (int i = 0; i < shown; i++) {
        const int x = tmap(g_cues[i].t, dur, bx0, bx1);
        kiln_gui_rect(x, by + 5, 1, 4, amber);
    }

    // The playhead last, so it is never hidden by a tick it sits on.
    const int px = tmap(t, dur, bx0, bx1);
    kiln_gui_rect(px, by - 5, 1, 14, RGBA32(120, 255, 160, 255));

    // ── The bottom row: pose, legend, or the last cues ─────────────────
    if (pm_cine_detached()) {
        // THE AUTHORING LOOP. Fly to a framing, read this line, type it into
        // the shot's PMCamKey table. Printed continuously rather than latched
        // behind a button, so ANY screenshot of this mode carries the numbers —
        // which is what makes the loop work from a capture rather than only
        // from a controller.
        //
        // Plain text, not a C literal: CLAUDE.md records that
        // FONT_BUILTIN_DEBUG_MONO has no '@' glyph and renders it as '0', so a
        // label reading "2@5.4" came out as "205.4" and read as a coordinate.
        // Rather than find out which other glyphs are missing the hard way,
        // this stays alphanumeric plus '.' and '-'.
        fm_vec3_t lk;
        fly_look(&lk);
        kiln_gui_text(6, y0 + 28, RGBA32(160, 255, 180, 255),
                     "KEY t %.2f eye %.0f %.0f %.0f look %.0f %.0f %.0f",
                     t, g_fly_pos.v[0], g_fly_pos.v[1], g_fly_pos.v[2],
                     lk.v[0], lk.v[1], lk.v[2]);
    } else if (g_legend_left > 0.0f) {
        kiln_gui_text(6, y0 + 28, val,
                     "START pause  R step  L rate  DL DR seek  DU DD key  Z fly");
    } else {
        // The last three cues, most recent first — the answer to "did the
        // sound fire before the motor".
        char buf[16];
        int x = 6;
        for (int i = 0; i < 3 && i < shown; i++) {
            const PMCue *c = &g_cues[(g_cue_count - 1 - i + CUE_CAP * 2)
                                     % CUE_CAP];
            kiln_gui_text(x, y0 + 28, i == 0 ? amber : val, "%.2f %s", c->t,
                         cue_short(c->tag, buf, sizeof buf));
            x += 100;
        }
    }
}

// ── The lint ROM ───────────────────────────────────────────────────────
#ifndef PM_CINE_LINT
#define PM_CINE_LINT 0
#endif

int pm_cine_lint_mode(void) { return PM_CINE_LINT; }

static PMCineReport g_report[sizeof pm_cine_shots / sizeof pm_cine_shots[0]];
static uint8_t      g_no_cam[sizeof pm_cine_shots / sizeof pm_cine_shots[0]];
static int          g_report_ready;

void pm_cine_lint_run(void)
{
    // The lab's measured extents, for the three shots that happen inside it.
    // From pm_lab.h, which derives them from the header dank_lab_gen.py emits
    // by measuring the mesh it built — so this bounds the eye against the room
    // that actually ships, not against a number typed here.
    PMCineBounds lab;
    lab.mins = (fm_vec3_t){{ PM_LAB_REAL_X0, PM_LAB_REAL_Y0, PM_LAB_REAL_Z0 }};
    lab.maxs = (fm_vec3_t){{ PM_LAB_REAL_X1, PM_LAB_REAL_Y1, PM_LAB_REAL_Z1 }};
    lab.valid = 1;

    for (int i = 0; i < pm_cine_shot_count; i++) {
        const PMDemoShot *s = pm_cine_shots[i].shot;

        // One line per shot BEFORE its setup runs, so if a setup ever asserts
        // or runs the heap out, the last line printed names the shot that did
        // it. This walk plays every shot's setup in one burst, which leaves the
        // whole game's models resident at once — the same total residency a
        // full playthrough reaches (pm_models caches and teardown does not
        // unload), but arrived at in three seconds instead of twenty minutes.
        debugf("pm_cine_lint: %s\n", pm_cine_shots[i].name);

        // A shot with no draw callback has NO 3D PASS — pm_demo_draw is a
        // no-op for it — so it has no camera to validate and no keys by
        // design. NARRATION and CREDITS are both of these: text and video
        // over pm_env's clear colour. Linting them reported NO_KEYS, which is
        // a true statement about the data and a false one about the game, and
        // a validator that cries wolf on two of nine shots is one nobody
        // reads.
        //
        // DERIVED from the shot rather than declared in PM_SHOT_LIST beside
        // in_lab, because a hand-kept "this one has no camera" column is
        // exactly the parallel table this file's own header explains has
        // drifted twice. Give one of these a .draw and it starts being linted
        // the same frame, with no edit here.
        if (!s->draw) {
            g_no_cam[i] = 1;
            continue;
        }

        // setup() is REQUIRED, not optional. FLYOVER_KEYS is filled by
        // flyover_build_keys during pm_demo_play, and is all zeroes before it —
        // linting it cold would report a fourteen-key table of origins, which
        // is a defect in the harness dressed as a defect in the shot.
        //
        // Played as a ONE-SHOT. The flyover is the only shot the game ever
        // loops, and looping changes the curve (pm_camkey_sample wraps the
        // neighbour keys instead of clamping) — but only at the seam, and the
        // flyover's last key is a copy of its first precisely so that seam is
        // continuous. Its looped wrap is therefore the one thing this report
        // does not cover.
        pm_demo_play(s, 0);

        PMCineShot d;
        d.keys = s->keys;
        d.key_count = s->key_count;
        d.duration = s->duration;
        // RESOLVED, exactly as pm_demo_apply_frustum resolves them. Validating
        // the raw 0 a shot leaves in the struct would check a value the
        // renderer never sees.
        d.near_z = s->near_z > 0.0f ? s->near_z : PM_SHOT_NEAR_Z;
        d.far_z  = s->far_z  > 0.0f ? s->far_z  : PM_SHOT_FAR_Z;
        d.loop = 0;

        pm_cine_lint(&d, pm_cine_shots[i].in_lab ? &lab : NULL, &g_report[i]);
        pm_demo_stop();
    }
    g_report_ready = 1;
}

/** Draws the set bits of `mask` as names from `x`, and RETURNS the x it
 *  stopped at so errors and notes can share one row without overlapping.
 *  They share a row because the numbers row is already 34 characters wide at
 *  6 px a character — drawing flags beside it put "no keys" straight through
 *  the `over` and `spd` columns, which is how the first capture of this
 *  report came out reading "0mokeys1.0". */
static int draw_flags(int x, int y, uint32_t mask, int is_err)
{
    const color_t bad  = RGBA32(255, 96, 96, 255);
    const color_t note = RGBA32(255, 200, 80, 255);
    if (!mask) return x;
    for (uint32_t b = 1u; b; b <<= 1) {
        if (!(mask & b)) continue;
        const char *n = is_err ? pm_cine_err_name(b) : pm_cine_note_name(b);
        kiln_gui_text(x, y, is_err ? bad : note, "%s", n);
        x += 6 * (int)strlen(n) + 6;
    }
    return x;
}

void pm_cine_lint_draw(int w, int h)
{
    (void)h;
    const color_t bg   = RGBA32(0, 0, 0, 220);
    const color_t bord = RGBA32(60, 70, 90, 220);
    const color_t key  = RGBA32(120, 200, 255, 255);
    const color_t val  = RGBA32(220, 220, 220, 255);
    const color_t bad  = RGBA32(255, 96, 96, 255);
    const color_t dim  = RGBA32(130, 130, 130, 255);

    if (!g_report_ready) return;

    kiln_gui_panel(2, 2, w - 4, 232, bg, bord);
    kiln_gui_text(6, 12, key, "PM CINE LINT   shot     keys  dur   over  spd");

    int errs = 0;
    int y = 24;
    for (int i = 0; i < pm_cine_shot_count; i++) {
        const PMCineReport *r = &g_report[i];
        const PMDemoShot *s = pm_cine_shots[i].shot;

        if (g_no_cam[i]) {
            // Listed, not silently omitted: a shot missing from the report
            // reads as a shot missing from the registry, which is the one
            // thing PM_SHOT_LIST exists to make impossible.
            kiln_gui_text(6, y, dim, "%-10s %3d %6.1f    no 3D pass",
                         pm_cine_shots[i].name, s->key_count, s->duration);
            y += 22;
            continue;
        }
        if (r->err) errs++;

        kiln_gui_text(6, y, r->err ? bad : val, "%-10s %3d %6.1f %5.2f %5.1f",
                     pm_cine_shots[i].name, s->key_count, s->duration,
                     r->overshoot, r->speed_ratio);
        y += 10;
        // Both flag sets on the row BELOW the numbers, errors first, indented
        // under the shot name so a row's two lines read as one entry.
        int fx = draw_flags(20, y, r->err, 1);
        fx = draw_flags(fx, y, r->note, 0);
        // The index is what makes either one actionable. "overshoot 1.43" is
        // a fact about a shot you then have to bisect by hand; "overshoot
        // 1.43 at k11" names the two keys to look between. Both numbers are
        // already in the report — they were just not being shown.
        if (r->err)
            kiln_gui_text(fx, y, bad, "k%d", r->bad_key);
        else if (r->note & PM_CINE_NOTE_OVERSHOOT)
            kiln_gui_text(fx, y, RGBA32(255, 200, 80, 255), "k%d",
                         r->overshoot_seg);
        y += 12;
    }

    kiln_gui_text(6, 226, errs ? bad : RGBA32(140, 255, 160, 255),
                 "%d of %d shots have hard failures", errs, pm_cine_shot_count);
}

#endif // KILN_DEBUG
