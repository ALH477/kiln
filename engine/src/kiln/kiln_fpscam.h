/* SPDX-License-Identifier: MIT
 *
 * kiln_fpscam.h — first-person camera. A Quake/Doom-style eye camera with
 * yaw + pitch, not the OoT spring-arm follow that kiln_camera implements.
 *
 * ── Why a separate module, not a new kiln_camera mode ───────────────────
 * kiln_camera's entire geometry is built around a boom behind a target
 * actor: distance, height, look_height, spring-arm damping, collision
 * ray on the boom. A first-person camera has none of those — the camera
 * IS the player, there is no boom, and the look direction comes from
 * yaw + pitch (kiln_camera is yaw-only). Bolting FPS onto kiln_camera
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
 * Horizontal movement is kiln_clip_slide with the player's AABB, same
 * as before. Vertical movement is integrated separately: gravity pulls
 * vy down each frame, jump sets vy to jump_speed, and a kiln_clip_ground
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
 * Unlike kiln_camera, there is no mode stack. A first-person game that
 * wants a cutscene or a third-person moment can use kiln_camera
 * alongside this module and switch which one writes to KilnScene, or
 * simply override KilnScene fields directly for the duration.
 */
#ifndef KILN_FPSCAM_H
#define KILN_FPSCAM_H

#include <stdint.h>
#include <t3d/t3dmath.h>

#include "kiln_engine.h"
#include "kiln_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Pitch clamp in radians (~84 degrees). Prevents gimbal-flip. */
#define KILN_FPSCAM_PITCH_LIMIT 1.47f

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
    uint8_t   on_ground;   /**< kiln_clip_ground hit last frame                 */
    uint8_t   last_surf;   /**< hitsurface underfoot                          */
    fm_vec3_t mins;        /**< player AABB mins (relative to center)        */
    fm_vec3_t maxs;        /**< player AABB maxs (relative to center)        */
} KilnFpsCam;

/** Sane FPS defaults: 80 u/s walk, 140 u/s run, 0.05 rad/look,
 *  3-unit eye height, gravity 540, jump 180, AABB 8×8×24.
 *  Does NOT set pos/yaw/pitch — set those after init. */
void kiln_fpscam_init(KilnFpsCam *cam);

/** Snap position + angles directly (no damping — there is no damper).
 *  Use on level load and after respawn/teleport. */
void kiln_fpscam_snap(KilnFpsCam *cam, fm_vec3_t pos, float yaw, float pitch);

/** Advance the camera one frame: read the C-stick for look, the main
 *  stick for movement, integrate via kiln_clip_slide. `dt` is seconds.
 *  `in` is the joypad state for this frame (from kiln_input_get). */
void kiln_fpscam_update(KilnFpsCam *cam, const KilnInput *in, float dt);

/** Write eye + look-at into the scene's camera fields. Does not call
 *  kiln_scene_update — the caller still owns when matrices rebuild. */
void kiln_fpscam_apply(const KilnFpsCam *cam, KilnScene *scene);

/** Compute the forward (look) direction from yaw + pitch. */
fm_vec3_t kiln_fpscam_forward(const KilnFpsCam *cam);

/** Compute the right (strafe) vector from yaw (horizontal only). */
fm_vec3_t kiln_fpscam_right(const KilnFpsCam *cam);

#ifdef __cplusplus
}
#endif

#endif /* KILN_FPSCAM_H */