/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_fpscam.h — first-person camera. A Quake/Doom-style eye camera with
 * yaw + pitch, not the OoT spring-arm follow that m64_camera implements.
 *
 * ── Why a separate module, not a new m64_camera mode ───────────────────
 * m64_camera's entire geometry is built around a boom behind a target
 * actor: distance, height, look_height, spring-arm damping, collision
 * ray on the boom. A first-person camera has none of those — the camera
 * IS the player, there is no boom, and the look direction comes from
 * yaw + pitch (m64_camera is yaw-only). Bolting FPS onto m64_camera
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
 * Horizontal movement is m64_clip_slide with the player's AABB, same
 * as before. Vertical movement is integrated separately: gravity pulls
 * vy down each frame, jump sets vy to jump_speed, and a m64_clip_ground
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
 * Unlike m64_camera, there is no mode stack. A first-person game that
 * wants a cutscene or a third-person moment can use m64_camera
 * alongside this module and switch which one writes to M64Scene, or
 * simply override M64Scene fields directly for the duration.
 */
#ifndef M64_FPSCAM_H
#define M64_FPSCAM_H

#include <t3d/t3dmath.h>

#include "m64_engine.h"
#include "m64_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Pitch clamp in radians (~84 degrees). Prevents gimbal-flip. */
#define M64_FPSCAM_PITCH_LIMIT 1.47f

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
    uint8_t   on_ground;   /**< m64_clip_ground hit last frame                 */
    uint8_t   last_surf;   /**< hitsurface underfoot                          */
    fm_vec3_t mins;        /**< player AABB mins (relative to center)        */
    fm_vec3_t maxs;        /**< player AABB maxs (relative to center)        */
} M64FpsCam;

/** Sane FPS defaults: 80 u/s walk, 140 u/s run, 0.05 rad/look,
 *  3-unit eye height, gravity 540, jump 180, AABB 8×8×24.
 *  Does NOT set pos/yaw/pitch — set those after init. */
void m64_fpscam_init(M64FpsCam *cam);

/** Snap position + angles directly (no damping — there is no damper).
 *  Use on level load and after respawn/teleport. */
void m64_fpscam_snap(M64FpsCam *cam, fm_vec3_t pos, float yaw, float pitch);

/** Advance the camera one frame: read the C-stick for look, the main
 *  stick for movement, integrate via m64_clip_slide. `dt` is seconds.
 *  `in` is the joypad state for this frame (from m64_input_get). */
void m64_fpscam_update(M64FpsCam *cam, const M64Input *in, float dt);

/** Write eye + look-at into the scene's camera fields. Does not call
 *  m64_scene_update — the caller still owns when matrices rebuild. */
void m64_fpscam_apply(const M64FpsCam *cam, M64Scene *scene);

/** Compute the forward (look) direction from yaw + pitch. */
fm_vec3_t m64_fpscam_forward(const M64FpsCam *cam);

/** Compute the right (strafe) vector from yaw (horizontal only). */
fm_vec3_t m64_fpscam_right(const M64FpsCam *cam);

#ifdef __cplusplus
}
#endif

#endif /* M64_FPSCAM_H */