/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_camera.h — the OoT-style third-person follow camera. See CLAUDE.md's
 * Phase B notes for what was and wasn't carried over from Ocarina of Time's
 * camera system.
 *
 * ── Spring arm, not a rigid offset ─────────────────────────────────────
 * examples/actors-demo hard-codes `cam_pos = player.pos + (0,60,-110)` —
 * correct for a demo about the actor system, wrong for a game: it snaps
 * instantly to every player move and never turns to face where the player
 * is heading. OoT's follow cameras (Camera_Normal1 and kin) instead keep a
 * boom of fixed length behind the player and let its heading *lag* the
 * player's facing direction, so a sharp turn swings the camera around over
 * several frames instead of teleporting it. M64Camera reproduces that lag
 * with two independent dampers — one for the boom's yaw, one for the eye
 * position — rather than one, because OoT's own camera splits them too: the
 * boom's heading is genuinely slower to correct than the eye's slide, and
 * collapsing them into one damped value makes fast player turns feel like
 * the camera position is skating rather than swinging.
 *
 * ── Why linear-per-frame damping, not exp(-t) ──────────────────────────
 * A textbook critically-damped spring reaches for expf() every frame. This
 * engine's stance (see CLAUDE.md "Constraints") is single precision and no
 * gratuitous libm on a 93.75 MHz VR4300; `t = min(1, speed*dt)` applied to
 * fm_vec3_lerp/fm_lerp_angle is the same shape of curve (fast initial
 * correction, asymptotic settle) for one multiply and a compare instead of
 * a transcendental call, and at 60 fps with dt roughly constant the visual
 * difference is not perceptible. Not a general-purpose spring — do not
 * reuse this damping trick somewhere dt varies wildly frame to frame.
 *
 * ── What was NOT carried over from OoT ──────────────────────────────────
 * No collision-aware boom: OoT's camera raycasts against the BG collision
 * mesh and pulls the eye in when a wall would clip it. This engine has no
 * collision system yet (see CLAUDE.md "Not yet built"), so M64Camera can
 * and will clip through geometry. No mode stack (OoT switches whole camera
 * *modes* — normal, targeting, cutscene — pushed/popped on a stack); this
 * is one mode. A game wanting Z-targeting or cutscene cameras layers that
 * on top by driving M64Camera's target_pos/target_yaw from whichever mode
 * is active, or bypassing it and writing M64Scene's cam_pos/cam_target
 * directly for that mode.
 */
#ifndef M64_CAMERA_H
#define M64_CAMERA_H

#include <t3d/t3dmath.h>

#include "m64_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Boom geometry, in world units behind/above the target. */
    float distance;    /**< horizontal distance behind the target's yaw */
    float height;      /**< eye height above the target's position */
    float look_height; /**< look-at point height above the target's position;
                         *   separate from `height` so the camera can look
                         *   slightly down at the target rather than level */

    /* Damping speeds: larger settles faster. 1/seconds-to-mostly-converge,
     * not a physical spring constant — see the file comment. */
    float pos_speed; /**< eye + look-at position damper */
    float yaw_speed; /**< boom heading damper (usually slower than pos_speed
                       *   — see Camera_Normal1 in the OoT decomp for why a
                       *   trailing boom heading reads as "following", while
                       *   a boom that turns as fast as the eye slides reads
                       *   as rigidly locked to the player) */

    /* Smoothed state, updated in place each frame. */
    float yaw;        /**< current boom heading, radians */
    fm_vec3_t eye;     /**< current smoothed camera position */
    fm_vec3_t look;    /**< current smoothed look-at point */
} M64Camera;

/** Sane OoT-ish defaults: 6 unit boom, eye above the target, looking at
 *  chest height, pos_speed settling in a few frames, yaw_speed slower. Does
 *  NOT set eye/look/yaw — call m64_camera_snap once with a real target
 *  before the first m64_camera_apply, or those start at the origin. */
void m64_camera_init(M64Camera *cam);

/** Teleport the smoothed state directly behind `target_pos` facing
 *  `target_yaw`, with no damping. Use on level/room load and after any
 *  hard cut (respawn, warp) — feeding a teleport through the damper instead
 *  makes the camera visibly swim across the whole level for one frame. */
void m64_camera_snap(M64Camera *cam, fm_vec3_t target_pos, float target_yaw);

/** Advance the dampers one frame toward the boom implied by target_pos +
 *  target_yaw. `target_yaw` is the target's facing direction in radians
 *  (0 = +Z, matching M64Actor's yaw convention); the boom trails behind it
 *  by `distance` along -facing. */
void m64_camera_update(M64Camera *cam, fm_vec3_t target_pos, float target_yaw, float dt);

/** Write the smoothed eye/look-at into a scene's camera fields. Does not
 *  call m64_scene_update — the caller still owns when the projection/view
 *  matrices get rebuilt, same division of labour as every other scene
 *  field. */
void m64_camera_apply(const M64Camera *cam, M64Scene *scene);

#ifdef __cplusplus
}
#endif

#endif /* M64_CAMERA_H */
