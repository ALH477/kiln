/* SPDX-License-Identifier: MIT
 *
 * kiln_camera.h — the OoT-style third-person follow camera. See CLAUDE.md's
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
 * several frames instead of teleporting it. KilnCamera reproduces that lag
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
 * stack. kiln_camera_push/pop reproduces that: push saves the current mode
 * + the smoothed state (eye/look/yaw) on a fixed 4-deep stack and switches
 * to the new mode; pop restores. NORMAL damps toward the boom behind the
 * target as before. TARGETING orbits so the locked actor and the targeter
 * are both in frame. CUTSCENE holds a fixed eye/look pair the caller drives
 * from C events. Backward compatible: if no mode is ever pushed, the
 * camera stays in NORMAL and behaves exactly like Phase B.
 *
 * ── Collision-aware boom (Phase 5) ─────────────────────────────────────
 * OoT raycasts the boom against BG collision and pulls the eye in before a
 * wall would clip it. With kiln_clip now in the engine, the camera can do
 * the same: each frame, after computing desired_eye, if
 * `collision_enabled` is set, kiln_clip_ray from look to desired_eye; on a
 * hit, the eye moves to endpos (less a small margin so the camera doesn't
 * sit exactly on the wall plane and jitter). Default OFF so existing
 * examples don't change behaviour; opt in with kiln_camera_set_collision.
 *
 * ── KILN_CAM_BOARD (Phase 4, party-game board view) ─────────────────────
 * The other three modes all frame ONE thing — a player, a lock-on pair, a
 * scripted shot. A party-game board camera frames a *region*: the whole
 * board when nothing is happening, tightening onto the active token while
 * it moves, then pulling back out. Doing that with NORMAL means feeding it
 * a fake target whose position and boom length you recompute every frame,
 * which is exactly the kind of caller-side hack a mode exists to avoid.
 *
 * BOARD holds a centre + a bounding radius (kiln_board's aabb_min/max feeds
 * this directly) and an orbit angle, and derives the eye from a pitch and
 * a fit distance computed once at set time from the scene's FOV — the one
 * trig-heavy step is not in the frame loop. `focus` in [0,1] lerps the
 * look-at from the board centre toward the update()'s target_pos and
 * shortens the boom, so the same mode covers both the wide establishing
 * shot and the tight follow with no push/pop churn between them. `spin`
 * drifts the orbit angle for the design's "wobbly camera" feel; set it to
 * 0 for a static board.
 *
 * Deliberately NOT a true orthographic isometric projection: KilnScene
 * builds a perspective matrix, and swapping in an ortho projection for one
 * camera mode would mean a second projection path through the whole scene
 * layer for a look a high-pitch perspective camera approximates closely
 * enough at board scale.
 */
#ifndef KILN_CAMERA_H
#define KILN_CAMERA_H

#include <t3d/t3dmath.h>

#include "kiln_engine.h"
#include "kiln_actor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KILN_CAM_NORMAL = 0,
    KILN_CAM_TARGETING,
    KILN_CAM_CUTSCENE,
    KILN_CAM_BOARD,
} KilnCamMode;

#define KILN_CAM_STACK_DEPTH 4

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
    KilnCamMode mode;
    int collision_enabled;
    KilnActorHandle target_actor;   /**< for TARGETING mode            */
    fm_vec3_t cutscene_eye;        /**< for CUTSCENE mode overrides  */
    fm_vec3_t cutscene_look;

    /* KILN_CAM_BOARD state. board_dist/sin_pitch/cos_pitch are derived once
     * by kiln_camera_set_board so the per-frame path is one sin/cos pair for
     * the orbit angle and no FOV trig at all. */
    fm_vec3_t board_center;
    float board_radius;
    float board_dist;      /**< fit distance derived from radius + FOV */
    float board_sin_pitch, board_cos_pitch;
    float board_orbit;     /**< current orbit angle, radians           */
    float board_spin;      /**< orbit drift, radians/second (0 = static) */
    float board_focus;     /**< 0 = whole board, 1 = on target_pos      */
    float board_focus_target;
    float board_focus_speed;

    struct {
        KilnCamMode mode;
        KilnActorHandle target_actor;
        fm_vec3_t eye;
        fm_vec3_t look;
        float yaw;
    } stack[KILN_CAM_STACK_DEPTH];
    int stack_depth;
} KilnCamera;

/** Sane OoT-ish defaults: 6 unit boom, eye above the target, looking at
 *  chest height, pos_speed settling in a few frames, yaw_speed slower. Does
 *  NOT set eye/look/yaw — call kiln_camera_snap once with a real target
 *  before the first kiln_camera_apply, or those start at the origin.
 *  Mode starts in NORMAL, collision disabled, no target actor. */
void kiln_camera_init(KilnCamera *cam);

/** Teleport the smoothed state directly behind `target_pos` facing
 *  `target_yaw`, with no damping. Use on level/room load and after any
 *  hard cut (respawn, warp) — feeding a teleport through the damper instead
 *  makes the camera visibly swim across the whole level for one frame. */
void kiln_camera_snap(KilnCamera *cam, fm_vec3_t target_pos, float target_yaw);

/** Advance the dampers one frame toward the boom implied by target_pos +
 *  target_yaw. `target_yaw` is the target's facing direction in radians
 *  (0 = +Z, matching KilnActor's yaw convention); the boom trails behind it
 *  by `distance` along -facing. In TARGETING mode the boom orbits to keep
 *  the target_actor in frame; in CUTSCENE the eye/look are held at the
 *  cutscene override (no damping toward target_pos). When
 *  collision_enabled is set, the boom is raycast against the world brushes
 *  and pulled in on a hit. */
void kiln_camera_update(KilnCamera *cam, fm_vec3_t target_pos, float target_yaw, float dt);

/** Write the smoothed eye/look-at into a scene's camera fields. Does not
 *  call kiln_scene_update — the caller still owns when the projection/view
 *  matrices get rebuilt, same division of labour as every other scene
 *  field. */
void kiln_camera_apply(const KilnCamera *cam, KilnScene *scene);

/** Push a new camera mode, saving the current mode + smoothed state for a
 *  later kiln_camera_pop. Returns 0 on success, -1 if the stack is full.
 *  Mode-specific fields (target_actor for TARGETING, eye/look for
 *  CUTSCENE) should be set AFTER the push via the setters below. */
int kiln_camera_push(KilnCamera *cam, KilnCamMode mode);

/** Pop the top of the mode stack, restoring the saved mode + smoothed
 *  state. Returns 0 on success, -1 if the stack is empty (no-op). */
int kiln_camera_pop(KilnCamera *cam);

/** Set the locked actor for TARGETING mode. Pass KILN_ACTOR_HANDLE_NONE to
 *  release. */
void kiln_camera_set_target_actor(KilnCamera *cam, KilnActorHandle h);

/** Set the explicit eye/look for CUTSCENE mode. The camera damps toward
 *  these (so a scripted pan still moves smoothly) rather than teleporting. */
void kiln_camera_set_cutscene(KilnCamera *cam, fm_vec3_t eye, fm_vec3_t look);

/** Configure KILN_CAM_BOARD's framing. `center` and `radius` are the board's
 *  bounding sphere — KilnBoard's aabb_min/aabb_max midpoint and half-diagonal
 *  feed this directly. `pitch_deg` is how far above the board the eye sits
 *  (0 = level with it, 90 = straight down; 45-60 reads as isometric).
 *  `fov_deg` must match the KilnScene this camera is applied to, or the board
 *  will not fill the frame as intended. Does trig; call it on board load,
 *  not per frame. */
void kiln_camera_set_board(KilnCamera *cam, fm_vec3_t center, float radius,
                          float pitch_deg, float fov_deg);

/** Orbit drift rate in radians/second for KILN_CAM_BOARD. 0 holds the angle. */
void kiln_camera_set_board_spin(KilnCamera *cam, float radians_per_sec);

/** Where KILN_CAM_BOARD should settle between the wide board shot (0) and the
 *  update()'s target_pos (1). Damped, so a token starting to move can just
 *  request 1 and a token finishing can request 0. `speed` is a damper rate
 *  like pos_speed; pass <= 0 to keep the current one (default 2.0). */
void kiln_camera_set_board_focus(KilnCamera *cam, float focus, float speed);

/** Teleport KILN_CAM_BOARD's smoothed eye/look to its current framing, with
 *  no damping. The BOARD-mode analogue of kiln_camera_snap — use it right
 *  after a push into BOARD so the camera doesn't swim in from wherever the
 *  previous mode left it. */
void kiln_camera_snap_board(KilnCamera *cam, fm_vec3_t target_pos);

/** Enable or disable the collision-aware boom. Off by default. When on,
 *  every kiln_camera_update raycasts look→desired_eye and pulls the eye in
 *  on a hit. Needs kiln_clip_set_world to have been called. */
void kiln_camera_set_collision(KilnCamera *cam, int enabled);

#ifdef __cplusplus
}
#endif

#endif /* KILN_CAMERA_H */