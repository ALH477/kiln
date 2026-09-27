/* SPDX-License-Identifier: MIT
 *
 * kiln_fpscam.h — first-person camera. A Quake/Doom-style eye camera with
 * yaw + pitch, not the OoT spring-arm follow that fig_camera implements.
 *
 * ── Why a separate module, not a new fig_camera mode ───────────────────
 * fig_camera's entire geometry is built around a boom behind a target
 * actor: distance, height, look_height, spring-arm damping, collision
 * ray on the boom. A first-person camera has none of those — the camera
 * IS the player, there is no boom, and the look direction comes from
 * yaw + pitch (fig_camera is yaw-only). Bolting FPS onto fig_camera
 * would leave most of its struct unused and its update logic bypassed,
 * which is harder to read than a 60-line module that does the one job.
 *
 * ── Look: C-stick, move: main stick ────────────────────────────────────
 * The N64 controller's main stick drives forward/back and strafe; the
 * C-stick (or C-buttons) drives look. This matches the GoldenEye/
 * Perfect Dark default scheme. Pitch is clamped to ±~84° to avoid the
 * gimbal-flip at ±90° where the forward vector collapses to a unit Y
 * and the right vector becomes undefined.
 *
 * ── Movement: horizontal slide + vertical gravity ──────────────────────
 * Horizontal movement is fig_clip_slide with the player's AABB, same
 * as before. Vertical movement is integrated separately: gravity pulls
 * vy down each frame, jump sets vy to jump_speed, and a fig_clip_ground
 * probe detects the floor. The split keeps wall sliding and floor
 * landing from interfering — a single combined slide would let the
 * player slide up walls when jumping beside them.
 *
 * ── Run ────────────────────────────────────────────────────────────────
 * Hold R to sprint at run_speed (default 140 u/s) instead of move_speed
 * (default 80). The N64's R button is the natural sprint modifier in
 * an FPS since it's not used for aiming (C-stick handles that).
 *
 * ── No mode stack ──────────────────────────────────────────────────────
 * Unlike fig_camera, there is no mode stack. A first-person game that
 * wants a cutscene or a third-person moment can use fig_camera
 * alongside this module and switch which one writes to FigScene, or
 * simply override FigScene fields directly for the duration.
 */
#ifndef FIG_FPSCAM_H
#define FIG_FPSCAM_H


/* The prefix migration train (docs/NAMING.md section 9 step 2). Pulled in by
 * every public header (a quoted include, so it resolves both in this tree and
 * in the installed include/kiln prefix) rather than force-included by
 * kiln-inst.mk, because a
 * force-include only reaches builds that include that file — a Nix check or a
 * host build compiling a downstream's sources directly never saw it, and
 * PetaByte-Madness' pm-cine check is what proved that. Deleting the train is
 * still a scripted one-line removal from these headers plus the file itself.
 */
#include "kiln_compat.h"

#include <stdint.h>
#include <t3d/t3dmath.h>

#include "kiln_engine.h"
#include "kiln_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Pitch clamp in radians (~84 degrees). Prevents gimbal-flip. */
#define FIG_FPSCAM_PITCH_LIMIT 1.47f

typedef struct {
    fm_vec3_t pos;         /**< eye position (world space)                    */
    float     yaw;         /**< horizontal heading, radians (0 = +Z)         */
    float     pitch;       /**< vertical angle, radians (clamped ±limit)     */
    float     move_speed;  /**< walk speed at full stick deflection           */
    float     run_speed;   /**< sprint speed (hold R)                         */
    float     look_speed;  /**< radians per unit stick deflection per frame   */
    float     eye_height;  /**< height of the eye above pos.v[1] (unused by  */
                            /*   the module itself; game can read it)          */
    float     gravity;     /**< vertical acceleration, units/sec^2 (0=none)  */
    float     jump_speed;  /**< initial vy on jump                            */
    float     vy;          /**< current vertical velocity                      */
    uint8_t   on_ground;   /**< fig_clip_ground hit last frame                 */
    uint8_t   last_surf;   /**< hitsurface underfoot                          */
    fm_vec3_t mins;        /**< player AABB mins (relative to center)        */
    fm_vec3_t maxs;        /**< player AABB maxs (relative to center)        */

    /** Which buttons sprint and jump. FIG_BTN_* masks, or 0 to disable that
     *  action entirely.
     *
     *  These were hardcoded to R and B, which is fine until a game wants
     *  those buttons for something else — and on this controller a game that
     *  wants an Ocarina-of-Time layout wants BOTH of them (R an arm weapon, B
     *  melee). A camera module has no business owning two of the four face
     *  buttons on everyone's behalf.
     *
     *  fig_fpscam_init sets them to R and B, so a caller that ignores these
     *  fields behaves exactly as before; overriding them after init is the
     *  whole interface. Two masks rather than a full binding table because
     *  these are the only two buttons this module reads — anything else a
     *  game binds, it binds itself, where it can see its own context. */
    uint32_t  btn_run;
    uint32_t  btn_jump;
} FigFpsCam;

/** Sane FPS defaults: 80 u/s walk, 140 u/s run, 0.05 rad/look,
 *  3-unit eye height, gravity 540, jump 180, AABB 8×8×24.
 *  Does NOT set pos/yaw/pitch — set those after init. */
void fig_fpscam_init(FigFpsCam *cam);

/** Snap position + angles directly (no damping — there is no damper).
 *  Use on level load and after respawn/teleport. */
void fig_fpscam_snap(FigFpsCam *cam, fm_vec3_t pos, float yaw, float pitch);

/** Advance the camera one frame: read the C-stick for look, the main
 *  stick for movement, integrate via fig_clip_slide. `dt` is seconds.
 *  `in` is the joypad state for this frame (from fig_input_get). */
void fig_fpscam_update(FigFpsCam *cam, const FigInput *in, float dt);

/** Write eye + look-at into the scene's camera fields. Does not call
 *  fig_scene_update — the caller still owns when matrices rebuild. */
void fig_fpscam_apply(const FigFpsCam *cam, FigScene *scene);

/** Compute the forward (look) direction from yaw + pitch. */
fm_vec3_t fig_fpscam_forward(const FigFpsCam *cam);

/** Compute the right (strafe) vector from yaw (horizontal only). */
fm_vec3_t fig_fpscam_right(const FigFpsCam *cam);

#ifdef __cplusplus
}
#endif

#endif /* FIG_FPSCAM_H */