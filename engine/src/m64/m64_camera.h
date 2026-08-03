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
 * ── Mode stack (Phase 5) ───────────────────────────────────────────────
 * OoT pushes whole camera *modes* — normal, targeting, cutscene — onto a
 * stack. m64_camera_push/pop reproduces that: push saves the current mode
 * + the smoothed state (eye/look/yaw) on a fixed 4-deep stack and switches
 * to the new mode; pop restores. NORMAL damps toward the boom behind the
 * target as before. TARGETING orbits so the locked actor and the targeter
 * are both in frame. CUTSCENE holds a fixed eye/look pair the caller drives
 * from C events. Backward compatible: if no mode is ever pushed, the
 * camera stays in NORMAL and behaves exactly like Phase B.
 *
 * ── Collision-aware boom (Phase 5) ─────────────────────────────────────
 * OoT raycasts the boom against BG collision and pulls the eye in before a
 * wall would clip it. With m64_clip now in the engine, the camera can do
 * the same: each frame, after computing desired_eye, if
 * `collision_enabled` is set, m64_clip_ray from look to desired_eye; on a
 * hit, the eye moves to endpos (less a small margin so the camera doesn't
 * sit exactly on the wall plane and jitter). Default OFF so existing
 * examples don't change behaviour; opt in with m64_camera_set_collision.
 */
#ifndef M64_CAMERA_H
#define M64_CAMERA_H

#include <t3d/t3dmath.h>

#include "m64_engine.h"
#include "m64_actor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    M64_CAM_NORMAL = 0,
    M64_CAM_TARGETING,
    M64_CAM_CUTSCENE,
} M64CamMode;

#define M64_CAM_STACK_DEPTH 4

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

    /* Phase 5 mode stack. `mode` is the active mode; the stack holds the
     * pushed frames so a pop restores mode + state together. */
    M64CamMode mode;
    int collision_enabled;
    M64ActorHandle target_actor;   /**< for TARGETING mode            */
    fm_vec3_t cutscene_eye;        /**< for CUTSCENE mode overrides  */
    fm_vec3_t cutscene_look;

    struct {
        M64CamMode mode;
        M64ActorHandle target_actor;
        fm_vec3_t eye;
        fm_vec3_t look;
        float yaw;
    } stack[M64_CAM_STACK_DEPTH];
    int stack_depth;
} M64Camera;

/** Sane OoT-ish defaults: 6 unit boom, eye above the target, looking at
 *  chest height, pos_speed settling in a few frames, yaw_speed slower. Does
 *  NOT set eye/look/yaw — call m64_camera_snap once with a real target
 *  before the first m64_camera_apply, or those start at the origin.
 *  Mode starts in NORMAL, collision disabled, no target actor. */
void m64_camera_init(M64Camera *cam);

/** Teleport the smoothed state directly behind `target_pos` facing
 *  `target_yaw`, with no damping. Use on level/room load and after any
 *  hard cut (respawn, warp) — feeding a teleport through the damper instead
 *  makes the camera visibly swim across the whole level for one frame. */
void m64_camera_snap(M64Camera *cam, fm_vec3_t target_pos, float target_yaw);

/** Advance the dampers one frame toward the boom implied by target_pos +
 *  target_yaw. `target_yaw` is the target's facing direction in radians
 *  (0 = +Z, matching M64Actor's yaw convention); the boom trails behind it
 *  by `distance` along -facing. In TARGETING mode the boom orbits to keep
 *  the target_actor in frame; in CUTSCENE the eye/look are held at the
 *  cutscene override (no damping toward target_pos). When
 *  collision_enabled is set, the boom is raycast against the world brushes
 *  and pulled in on a hit. */
void m64_camera_update(M64Camera *cam, fm_vec3_t target_pos, float target_yaw, float dt);

/** Write the smoothed eye/look-at into a scene's camera fields. Does not
 *  call m64_scene_update — the caller still owns when the projection/view
 *  matrices get rebuilt, same division of labour as every other scene
 *  field. */
void m64_camera_apply(const M64Camera *cam, M64Scene *scene);

/** Push a new camera mode, saving the current mode + smoothed state for a
 *  later m64_camera_pop. Returns 0 on success, -1 if the stack is full.
 *  Mode-specific fields (target_actor for TARGETING, eye/look for
 *  CUTSCENE) should be set AFTER the push via the setters below. */
int m64_camera_push(M64Camera *cam, M64CamMode mode);

/** Pop the top of the mode stack, restoring the saved mode + smoothed
 *  state. Returns 0 on success, -1 if the stack is empty (no-op). */
int m64_camera_pop(M64Camera *cam);

/** Set the locked actor for TARGETING mode. Pass M64_ACTOR_HANDLE_NONE to
 *  release. */
void m64_camera_set_target_actor(M64Camera *cam, M64ActorHandle h);

/** Set the explicit eye/look for CUTSCENE mode. The camera damps toward
 *  these (so a scripted pan still moves smoothly) rather than teleporting. */
void m64_camera_set_cutscene(M64Camera *cam, fm_vec3_t eye, fm_vec3_t look);

/** Enable or disable the collision-aware boom. Off by default. When on,
 *  every m64_camera_update raycasts look→desired_eye and pulls the eye in
 *  on a hit. Needs m64_clip_set_world to have been called. */
void m64_camera_set_collision(M64Camera *cam, int enabled);

#ifdef __cplusplus
}
#endif

#endif /* M64_CAMERA_H */