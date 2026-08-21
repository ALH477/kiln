// SPDX-License-Identifier: MPL-2.0
//
// pm_cine.h — a debugger for the cinematics.
//
// PetaByte Madness' intro is nine keyframed shots over live geometry, driven by
// one director (pm_demo.c). It is the most finished part of the game and, until
// this module, the least inspectable part of the codebase.
//
// ── The clock was not addressable ──────────────────────────────────────
// pm_demo.c's `g_elapsed` only ever counts up, at 1x. To look at second 22.7 of
// a 36-second shot you booted and waited 22.7 seconds of WALL CLOCK, on an
// emulator whose speed is not guaranteed and under a `./dev shot <rom> out.png
// 6` whose settle time is a guess about machine load. There was no pause, no
// step, no rate, no seek, and no way to jump to a key.
//
// So this module owns the shot clock:
//
//   pm_cine_dt()      scales it (pause / step / 0.25x-2x)
//   pm_cine_seek()    re-runs the shot deterministically to an exact time
//   PM_SHOT_AT=<sec>  does that at boot, which is what makes a capture
//                     REPRODUCIBLE rather than approximately timed
//
// ── Everything a shot DOES was invisible ───────────────────────────────
// pm_intake.c fires five cues and switches seven animation clips;
// pm_arrival.c's beach fires four punctuation beats with hit-stop and flashes.
// None of it left a trace, so "the flash lands after the fade" and "the sound
// fires before the motor" were judged by watching, twice, and remembering.
//
// The cue ring below fixes that, and it is filled from FUNNELS rather than from
// edits scattered through every cutscene: pm_fx's four entry points, pm_sfx's
// one, and each file's own play_once. Seven lines of instrumentation covers
// every cinematic in the game, including ones not written yet.
//
// ── Seeking is re-simulation, not rewinding ────────────────────────────
// A cinematic here is not a curve, it is a curve plus an animation state
// machine (pm_intake.c's IntakeState, pm_arrival.c's play_once) plus fx plus
// sound. You cannot rewind a state machine. So pm_cine_seek RESTARTS the shot
// and pumps it forward at a fixed step with sound muted — which is slower, and
// is the only version that is actually correct, and has the property that
// matters most for capture: it is identical every single time.
//
// ── Debug ROM only ─────────────────────────────────────────────────────
// Same contract as pm_debug.h: everything compiles to nothing without
// KILN_DEBUG, which nix/rom.nix's `debugConsole = true` defines. The shipping
// ROM pays nothing, not even the branch — pm_cine_dt becomes `(dt)`.
//
// Note the difference from the ENGINE's convention (kiln_debugdraw.h,
// kiln_console.h), where symbols are always present and only the call sites are
// gated. That rule exists because nix/engine.nix builds libkiln.a exactly once,
// without KILN_DEBUG, so a compiled-away engine module breaks every debug ROM's
// link. The game is built per-ROM, so it does not have that problem and can
// take the cheaper option.

#ifndef PM_CINE_H
#define PM_CINE_H

#include <kiln/kiln_engine.h>
#include <kiln/kiln_input.h>

#include "pm_demo.h"

// ── The registry ───────────────────────────────────────────────────────
// Every cinematic in the game, addressable by index. An X-macro for the reason
// PM_SCREEN_LIST and PM_MODEL_LIST are X-macros: a hand-kept table parallel to
// something it is not derived from has drifted twice in this codebase, and one
// of those drifts was a NULL format string at boot on every debug build.
//
// nix/checks/pm-cine.nix greps every `const PMDemoShot pm_...` definition in
// src/ and fails if one is missing from here, so "added a shot, forgot the
// registry" is not a reachable state.
//
// Order is display order, and it is roughly the order the game plays them.
//
// The third column is "this shot happens inside the lab, so its eye should stay
// inside the room the generator measured". It is what turns pm_cine_lint's
// containment check on, and it is a per-shot fact rather than something derived
// from PMDemoShot.exterior: NARRATION and CREDITS are neither interior nor
// exterior — they are text and video over a dead camera — and bounding them
// against a room they never draw would report a defect that is not one.
#define PM_SHOT_LIST(X)                          \
    X(FLYOVER,   &pm_demo_reel[0],     0)        \
    X(REEL_LAB,  &pm_demo_reel[1],     1)        \
    X(CENTAUR,   &pm_demo_reel[2],     0)        \
    X(NARRATION, &pm_narration_shot,   0)        \
    X(LAB_CINE,  &pm_demo_lab_cine,    1)        \
    X(INTAKE,    &pm_intake_shot,      1)        \
    X(CREDITS,   &pm_credits_shot,     0)        \
    X(SUB,       &pm_arrival_sub,      0)        \
    X(BEACH,     &pm_arrival_beach,    0)

#ifdef KILN_DEBUG

typedef struct {
    const char       *name;
    const PMDemoShot *shot;
    int               in_lab;   /**< bound the eye to the measured room */
} PMCineEntry;

extern const PMCineEntry pm_cine_shots[];
extern const int         pm_cine_shot_count;

// ── Transport ──────────────────────────────────────────────────────────

/** The dt the shot should advance by this frame: `dt` scaled by the current
 *  rate, 0 while paused, and exactly one frame's worth on a step.
 *
 *  Called at BOTH of pm_screens.c's clock sites — pm_demo_update and
 *  pm_fx_update. Feeding the director alone would let the letterbox and the
 *  flash keep running over a frozen scene, which is precisely backwards for
 *  studying a single frame of a flash.
 *
 *  Everything downstream stays in lockstep for free: pm_demo_update passes its
 *  dt straight through to `shot->update(elapsed, dt)`, and every shot feeds
 *  that same dt to kiln_skel_update and to its own state clock. There is no
 *  second clock to keep in sync. */
float pm_cine_dt(float dt);

/** Read the pad. Arms on L+R; see pm_cine.c for the full map, which is also
 *  drawn on screen so it never has to be remembered. */
void pm_cine_input(const KilnInput *in);

/** 1 while the transport owns the pad. main.c must then hand pm_screens_update
 *  a zeroed input: otherwise START skips the very shot being studied
 *  (pm_screens.c's intro_update), and D-up/D-down move a menu cursor. */
int pm_cine_grabs_input(void);

/** Per-frame bookkeeping: notice a shot change, run PM_SHOT_AT and the ladder,
 *  drive the free-fly camera. Call once per frame, AFTER pm_screens_update (so
 *  the director has already been advanced) and BEFORE the camera is applied to
 *  the scene, which is what pm_cine_override_camera then writes into.
 *
 *  Takes the REAL dt, not the transported one: the free-fly camera is the
 *  viewer's, and it has to keep moving while the shot it is inspecting is
 *  paused. That is most of the point of pausing. */
void pm_cine_frame(float dt);

/** Restart the current shot and pump it to `t` at a fixed step, with SFX muted
 *  and the fx state reset afterwards. Deterministic and repeatable; costs
 *  `t * 60` simulation steps, which is a visible hitch on a long shot and the
 *  price of the guarantee. */
void pm_cine_seek(float t);

/** 1 in a build whose whole purpose is a REPRODUCIBLE capture — currently
 *  `PM_SHOT_AT=<sec>`, which seeks to an exact shot time and pauses there.
 *
 *  Overlay code should read this and suppress anything that cannot be the same
 *  twice: a wall-clock uptime, a smoothed frame rate. Measured, not assumed —
 *  two captures of the same PM_SHOT_AT ROM differed in exactly 319 pixels, all
 *  of them inside pm_debug's `%.1fs` / `%.0f fps` fields, with the scene, the
 *  camera, the timeline and the cue trace already byte-identical. A capture
 *  that differs only in the field reporting how long it took to capture is one
 *  that cannot be diffed, for no information. */
int pm_cine_repro(void);

/** 1 while a seek is pumping. The cue ring keeps recording (the trace after a
 *  seek should be the full history up to `t`), but anything that would be
 *  absurd to do 2,000 times in one frame should ask. */
int pm_cine_seeking(void);

// ── The cue trace ──────────────────────────────────────────────────────

/** Record a named beat at the current shot time. `tag` must be a string
 *  LITERAL or otherwise outlive the trace — the ring stores the pointer and
 *  never copies, which is what keeps it free of allocation on a console that
 *  has none to spare. */
void pm_cine_cue(const char *tag);

/** Convenience for the funnels, so a call site is one short line. */
#define PM_CUE(tag) pm_cine_cue(tag)

// ── The detached camera ────────────────────────────────────────────────

/** 1 when the free-fly camera has taken the view. The shot keeps running and
 *  keeps writing its own pose; this just stops that pose being what the scene
 *  is built from. */
int pm_cine_detached(void);

/** Overwrite the scene's camera with the free-fly pose. Returns 1 if it did.
 *  No-op unless detached, so the call site in main.c needs no condition. */
int pm_cine_override_camera(KilnScene *scene);

/** Draw the SHOT's camera — gizmo, sightline and frustum — from wherever the
 *  free-fly is standing. Draws nothing unless detached, because undetached the
 *  shot camera IS the viewer and a frustum drawn from inside itself is a
 *  full-screen X.
 *
 *  Opens its own kiln_dd block rather than borrowing pm_debug's, deliberately:
 *  pm_debug's spatial layers default to OFF and cycle through sets that do not
 *  include this, and a detached camera with no frustum drawn is the single most
 *  confusing state this module can be in. Call it inside the GUI pass. */
void pm_cine_draw3d(const KilnScene *scene, int screen_w, int screen_h);

// ── The overlay ────────────────────────────────────────────────────────

/** Timeline strip, cue trace, button legend, and — while detached — the pose
 *  readout. Call inside the GUI pass. */
void pm_cine_draw(int screen_w, int screen_h);

// ── The lint ROM ───────────────────────────────────────────────────────

/** 1 in a `PM_CINE_LINT=1` build. main.c then skips the game entirely and
 *  shows the report, because the report is the whole point of that ROM. */
int pm_cine_lint_mode(void);

/** Walk PM_SHOT_LIST, running each shot's setup() -> pm_cine_lint() ->
 *  teardown(). setup() is required, not optional: FLYOVER_KEYS is filled by
 *  flyover_build_keys during setup and is all zeroes before it. */
void pm_cine_lint_run(void);

/** Draw the report: one row per shot, hard failures in red. Sized to be read
 *  off a single `./dev shot pm-cine-lint out.png 3`. */
void pm_cine_lint_draw(int screen_w, int screen_h);

#else

#define pm_cine_dt(dt)                  (dt)
#define pm_cine_input(in)               ((void)0)
#define pm_cine_grabs_input()           (0)
#define pm_cine_frame(dt)               ((void)0)
#define pm_cine_seek(t)                 ((void)0)
#define pm_cine_seeking()               (0)
#define pm_cine_repro()                 (0)
#define pm_cine_cue(tag)                ((void)0)
#define PM_CUE(tag)                     ((void)0)
#define pm_cine_detached()              (0)
#define pm_cine_override_camera(scene)  (0)
#define pm_cine_draw3d(scene, w, h)     ((void)0)
#define pm_cine_draw(w, h)              ((void)0)
#define pm_cine_lint_mode()             (0)
#define pm_cine_lint_run()              ((void)0)
#define pm_cine_lint_draw(w, h)         ((void)0)

#endif // KILN_DEBUG

#endif // PM_CINE_H
