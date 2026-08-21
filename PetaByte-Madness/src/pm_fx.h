// SPDX-License-Identifier: MPL-2.0
//
// pm_fx.h — the presentation layer: letterbox, shake, flash, hit-stop.
//
// The difference between a scene that happens and a scene that lands is
// almost never geometry. It is that the bars come in, the camera jolts on
// the impact, the frame holds for two hundred milliseconds on the hit, and
// the screen goes white for one. None of that costs triangles and all of
// it is what a 1998 console game spent its polish budget on.
//
// ── Everything here is 2D or a camera offset ───────────────────────────
// Nothing in this module touches the 3D pass, adds a draw call to a model,
// or allocates. Letterbox and flash are rectangles in the GUI pass; shake
// is three floats added to the scene's camera; hit-stop is a multiplier on
// dt. The most expensive thing here is the flash, and it is expensive for
// about a tenth of a second.
//
// ── Hit-stop is a time scale, not a pause ──────────────────────────────
// pm_fx_time_scale() is applied to the dt that gameplay and animation see,
// while the fx clock itself keeps running on real time. That split is the
// whole trick: the world stops but the shake and the flash do not, so a
// hit reads as impact rather than as a dropped frame.
//
// ── Shake is tabulated, not sinf ───────────────────────────────────────
// A shake that calls sinf three times a frame is three transcendentals for
// something nobody can distinguish from a lookup. Consistent with the
// engine's stance on libm (see kiln_camera.h on why its damping is linear
// rather than exp) and with VEIL_DESIGN.md's habit of pre-baking anything
// that can be pre-baked.

#ifndef PM_FX_H
#define PM_FX_H

#include <libdragon.h>
#include <t3d/t3dmath.h>
#include <kiln/kiln_engine.h>

/** Reset every effect. Call when a scene starts. */
void pm_fx_reset(void);

/** Advance on REAL time — never on the scaled dt, or hit-stop would
 *  freeze the very effects that sell the hit. */
void pm_fx_update(float dt);

// ── Letterbox ──────────────────────────────────────────────────────────
/** Drive the bars toward `target` (0 = none, 1 = full cinematic bars).
 *  They ease in and out over ~0.4 s rather than snapping, because a hard
 *  cut to bars reads as a glitch and an eased one reads as a film. */
void pm_fx_letterbox(float target);

/** 1 once the bars have finished coming in — for a shot that should not
 *  start until the frame is framed. */
int pm_fx_letterbox_settled(void);

// ── Shake ──────────────────────────────────────────────────────────────
/** Kick the camera. `amount` is in world units of peak displacement,
 *  `seconds` how long it decays over. A second call while one is running
 *  takes the STRONGER of the two rather than summing — two overlapping
 *  hits should not multiply into a seizure. */
void pm_fx_shake(float amount, float seconds);

/** Add the current shake offset to the scene's camera. Call after the
 *  camera has been applied and before kiln_scene_update, so the shake is
 *  in the matrices rather than fighting the damper that produced them. */
void pm_fx_apply_camera(KilnScene *scene);

// ── Flash ──────────────────────────────────────────────────────────────
/** Fill the screen with `c` and fall off over `seconds`. The alpha in `c`
 *  is the peak. */
void pm_fx_flash(color_t c, float seconds);

// ── Hit-stop ───────────────────────────────────────────────────────────
/** Freeze the world for `seconds` (typically 0.06-0.2). */
void pm_fx_hitstop(float seconds);

/** Multiply gameplay dt by this. 0 while a hit-stop is running, 1
 *  otherwise. */
float pm_fx_time_scale(void);

// ── The binary scroll ──────────────────────────────────────────────────
/** Run the MADNESS overlay for `seconds`: fast-scrolling binary with the
 *  word spelled in ASCII bytes, so it decodes if anyone pauses on it. */
void pm_fx_binary(float seconds);
int  pm_fx_binary_active(void);

/** Draw letterbox, flash and binary. Call LAST in the GUI pass — these sit
 *  over the HUD, not under it. */
void pm_fx_draw(int screen_w, int screen_h);

#endif // PM_FX_H
