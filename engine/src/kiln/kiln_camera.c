/* SPDX-License-Identifier: MIT
 *
 * kiln_camera.c — see kiln_camera.h for the model.
 */

#include "kiln_camera.h"
#include "kiln_clip.h"

#include <fmath.h>

static float damp_t(float speed, float dt)
{
    float t = speed * dt;
    return t < 1.0f ? t : 1.0f;
}

void kiln_camera_init(KilnCamera *cam)
{
    cam->distance = 6.0f;
    cam->height = 3.0f;
    cam->look_height = 1.5f;

    cam->pos_speed = 10.0f;
    cam->yaw_speed = 4.0f;

    cam->yaw = 0.0f;
    cam->eye = (fm_vec3_t){ { 0, 0, 0 } };
    cam->look = (fm_vec3_t){ { 0, 0, 0 } };

    cam->mode = KILN_CAM_NORMAL;
    cam->collision_enabled = 0;
    cam->target_actor = KILN_ACTOR_HANDLE_NONE;
    cam->cutscene_eye  = (fm_vec3_t){ { 0, 0, 0 } };
    cam->cutscene_look = (fm_vec3_t){ { 0, 0, 0 } };
    cam->stack_depth = 0;

    /* BOARD defaults. A zero radius would divide the fit distance to
     * nothing, so seed a usable board rather than a degenerate one; a
     * caller that pushes BOARD without set_board gets a sane wide shot
     * instead of the eye inside the look-at point. */
    cam->board_center = (fm_vec3_t){ { 0, 0, 0 } };
    cam->board_radius = 10.0f;
    cam->board_dist = 20.0f;
    cam->board_sin_pitch = 0.70710678f;   /* 45 degrees */
    cam->board_cos_pitch = 0.70710678f;
    cam->board_orbit = 0.0f;
    cam->board_spin = 0.0f;
    cam->board_focus = 0.0f;
    cam->board_focus_target = 0.0f;
    cam->board_focus_speed = 2.0f;
}

/** Boom position for a given target position + heading: `distance` behind
 *  the heading (heading 0 == +Z, matching KilnActor's yaw), `height` up. */
static fm_vec3_t boom_eye(fm_vec3_t target_pos, float yaw, float distance, float height)
{
    return (fm_vec3_t){ {
        target_pos.v[0] - fm_sinf(yaw) * distance,
        target_pos.v[1] + height,
        target_pos.v[2] - fm_cosf(yaw) * distance,
    } };
}

void kiln_camera_snap(KilnCamera *cam, fm_vec3_t target_pos, float target_yaw)
{
    cam->yaw = target_yaw;
    cam->eye = boom_eye(target_pos, target_yaw, cam->distance, cam->height);
    cam->look = (fm_vec3_t){ {
        target_pos.v[0], target_pos.v[1] + cam->look_height, target_pos.v[2],
    } };
}

/** Pull `eye` toward `look` so it doesn't sit on a wall plane. Done after
 *  the desired eye is computed in any mode; only acts when
 *  collision_enabled is set. */
static fm_vec3_t collide_boom(fm_vec3_t look, fm_vec3_t desired_eye)
{
    KilnTrace tr = kiln_clip_ray(look, desired_eye);
    if (tr.fraction >= 1.0f) return desired_eye;
    /* endpos sits exactly on the wall plane; back off a small margin so
     * the camera doesn't z-fight / jitter against it. 0.5 world units. */
    fm_vec3_t d = {{ desired_eye.v[0] - look.v[0],
                     desired_eye.v[1] - look.v[1],
                     desired_eye.v[2] - look.v[2] }};
    float frac = tr.fraction - 0.5f / (fm_vec3_len(&d) + 1e-3f);
    if (frac < 0.0f) frac = 0.0f;
    return (fm_vec3_t){ {
        look.v[0] + d.v[0] * frac,
        look.v[1] + d.v[1] * frac,
        look.v[2] + d.v[2] * frac,
    } };
}

static void update_normal(KilnCamera *cam, fm_vec3_t target_pos, float target_yaw, float dt)
{
    cam->yaw = fm_lerp_angle(cam->yaw, target_yaw, damp_t(cam->yaw_speed, dt));

    fm_vec3_t desired_eye = boom_eye(target_pos, cam->yaw, cam->distance, cam->height);
    fm_vec3_t desired_look = (fm_vec3_t){ {
        target_pos.v[0], target_pos.v[1] + cam->look_height, target_pos.v[2],
    } };
    if (cam->collision_enabled) desired_eye = collide_boom(desired_look, desired_eye);

    float t = damp_t(cam->pos_speed, dt);
    fm_vec3_lerp(&cam->eye, &cam->eye, &desired_eye, t);
    fm_vec3_lerp(&cam->look, &cam->look, &desired_look, t);
}

static void update_targeting(KilnCamera *cam, fm_vec3_t target_pos, float dt)
{
    /* Sit the eye on the far side of the targeter from the locked actor,
     * so both are in frame. Look at the midpoint of the two. A shorter
     * boom than NORMAL keeps the framing tight. */
    KilnActor *t = kiln_actor_resolve(cam->target_actor);
    if (!t) { update_normal(cam, target_pos, cam->yaw, dt); return; }

    fm_vec3_t tp = t->xform.pos;
    fm_vec3_t to_target = {{ tp.v[0] - target_pos.v[0],
                              tp.v[1] - target_pos.v[1],
                              tp.v[2] - target_pos.v[2] }};
    float tlen = fm_vec3_len(&to_target);
    if (tlen < 1e-3f) { update_normal(cam, target_pos, cam->yaw, dt); return; }
    fm_vec3_t tdir = {{ to_target.v[0] / tlen,
                        to_target.v[1] / tlen,
                        to_target.v[2] / tlen }};

    float dist = cam->distance * 1.2f;
    fm_vec3_t desired_eye = {{ target_pos.v[0] - tdir.v[0] * dist,
                                target_pos.v[1] + cam->height,
                                target_pos.v[2] - tdir.v[2] * dist }};
    fm_vec3_t desired_look = {{ (target_pos.v[0] + tp.v[0]) * 0.5f,
                                 (target_pos.v[1] + tp.v[1]) * 0.5f + cam->look_height,
                                 (target_pos.v[2] + tp.v[2]) * 0.5f }};
    if (cam->collision_enabled) desired_eye = collide_boom(desired_look, desired_eye);

    float t2 = damp_t(cam->pos_speed, dt);
    fm_vec3_lerp(&cam->eye, &cam->eye, &desired_eye, t2);
    fm_vec3_lerp(&cam->look, &cam->look, &desired_look, t2);
    /* Keep yaw tracking the line from targeter to locked actor, so a pop
     * back to NORMAL resumes from a sane heading. */
    cam->yaw = fm_atan2f(to_target.v[0], to_target.v[2]);
}

static void update_cutscene(KilnCamera *cam, float dt)
{
    (void)dt;
    fm_vec3_t desired_eye  = cam->cutscene_eye;
    fm_vec3_t desired_look = cam->cutscene_look;
    if (cam->collision_enabled) desired_eye = collide_boom(desired_look, desired_eye);

    /* A cutscene pose is FOLLOWED EXACTLY, not damped toward.
     *
     * This used to lerp with the same pos_speed damper the follow camera
     * uses, which is wrong in three compounding ways:
     *
     *   - It double-smooths. A scripted shot already interpolates between
     *     its own keyframes (smoothstepped, in PetaByte Madness' case), so
     *     damping that motion again only adds lag. At 60 fps and speed 10
     *     the camera closes 17% of the gap per frame, so it trails a
     *     moving shot by several frames and never arrives.
     *
     *   - The lag is on the LOOK target too, so a moving shot is not
     *     merely late, it is aimed somewhere the director did not choose.
     *
     *   - Worst: when one shot cuts to another, the damper TRAVELS between
     *     them. A cut from an aerial to a room interior became a half
     *     second of the camera flying across the world and through
     *     whatever was in the way, which reads as the camera spazzing and
     *     as geometry clipping through the lens.
     *
     * A cut should cut. If a shot wants easing it belongs in the shot's
     * own keyframes, where the author can see it. */
    cam->eye  = desired_eye;
    cam->look = desired_look;
}

/** BOARD framing for the current orbit/focus. Split out so update and snap
 *  agree by construction — a snap that recomputed the framing differently
 *  from update would pop on the very next frame. */
static void board_frame(const KilnCamera *cam, fm_vec3_t target_pos,
                        fm_vec3_t *out_eye, fm_vec3_t *out_look)
{
    float f = cam->board_focus;
    fm_vec3_t look = {{
        cam->board_center.v[0] + (target_pos.v[0] - cam->board_center.v[0]) * f,
        cam->board_center.v[1] + (target_pos.v[1] - cam->board_center.v[1]) * f,
        cam->board_center.v[2] + (target_pos.v[2] - cam->board_center.v[2]) * f,
    }};
    /* Fully focused sits at 45% of the wide shot's distance — close enough
     * to read a token's animation, far enough that the neighbouring spaces
     * stay on screen so a move still has context. */
    float dist = cam->board_dist * (1.0f - 0.55f * f);
    float horiz = dist * cam->board_cos_pitch;
    *out_look = look;
    *out_eye = (fm_vec3_t){ {
        look.v[0] - fm_sinf(cam->board_orbit) * horiz,
        look.v[1] + dist * cam->board_sin_pitch,
        look.v[2] - fm_cosf(cam->board_orbit) * horiz,
    } };
}

static void update_board(KilnCamera *cam, fm_vec3_t target_pos, float dt)
{
    cam->board_orbit += cam->board_spin * dt;
    cam->board_focus += (cam->board_focus_target - cam->board_focus) *
                        damp_t(cam->board_focus_speed, dt);

    fm_vec3_t desired_eye, desired_look;
    board_frame(cam, target_pos, &desired_eye, &desired_look);
    if (cam->collision_enabled) desired_eye = collide_boom(desired_look, desired_eye);

    float t = damp_t(cam->pos_speed, dt);
    fm_vec3_lerp(&cam->eye, &cam->eye, &desired_eye, t);
    fm_vec3_lerp(&cam->look, &cam->look, &desired_look, t);
    /* Keep yaw following the orbit so a pop back to NORMAL resumes from the
     * heading the player was last looking along, same contract as
     * update_targeting. */
    cam->yaw = cam->board_orbit;
}

void kiln_camera_update(KilnCamera *cam, fm_vec3_t target_pos, float target_yaw, float dt)
{
    switch (cam->mode) {
    case KILN_CAM_TARGETING: update_targeting(cam, target_pos, dt); break;
    case KILN_CAM_CUTSCENE:  update_cutscene(cam, dt); break;
    case KILN_CAM_BOARD:     update_board(cam, target_pos, dt); break;
    case KILN_CAM_NORMAL:
    default:                update_normal(cam, target_pos, target_yaw, dt); break;
    }
}

void kiln_camera_set_board(KilnCamera *cam, fm_vec3_t center, float radius,
                          float pitch_deg, float fov_deg)
{
    if (radius < 0.1f) radius = 0.1f;
    cam->board_center = center;
    cam->board_radius = radius;

    /* Fit distance: the eye must be far enough that a sphere of `radius`
     * subtends at most the FOV. d = r / sin(fov/2). Clamp the FOV to a sane
     * band first — a 0 or 180 degree FOV divides by ~0 and puts the eye at
     * infinity or inside the board. */
    if (fov_deg < 10.0f)  fov_deg = 10.0f;
    if (fov_deg > 150.0f) fov_deg = 150.0f;
    float half = fov_deg * 0.5f * 0.017453292f;
    float s = fm_sinf(half);
    cam->board_dist = radius / (s > 1e-3f ? s : 1e-3f);

    if (pitch_deg < 5.0f)  pitch_deg = 5.0f;
    if (pitch_deg > 85.0f) pitch_deg = 85.0f;
    float p = pitch_deg * 0.017453292f;
    cam->board_sin_pitch = fm_sinf(p);
    cam->board_cos_pitch = fm_cosf(p);
}

void kiln_camera_set_board_spin(KilnCamera *cam, float radians_per_sec)
{
    cam->board_spin = radians_per_sec;
}

void kiln_camera_set_board_focus(KilnCamera *cam, float focus, float speed)
{
    if (focus < 0.0f) focus = 0.0f;
    if (focus > 1.0f) focus = 1.0f;
    cam->board_focus_target = focus;
    if (speed > 0.0f) cam->board_focus_speed = speed;
}

void kiln_camera_snap_board(KilnCamera *cam, fm_vec3_t target_pos)
{
    cam->board_focus = cam->board_focus_target;
    board_frame(cam, target_pos, &cam->eye, &cam->look);
    cam->yaw = cam->board_orbit;
}

void kiln_camera_apply(const KilnCamera *cam, KilnScene *scene)
{
    scene->cam_pos = cam->eye;
    scene->cam_target = cam->look;
}

int kiln_camera_push(KilnCamera *cam, KilnCamMode mode)
{
    if (cam->stack_depth >= KILN_CAM_STACK_DEPTH) return -1;
    cam->stack[cam->stack_depth].mode = cam->mode;
    cam->stack[cam->stack_depth].target_actor = cam->target_actor;
    cam->stack[cam->stack_depth].eye = cam->eye;
    cam->stack[cam->stack_depth].look = cam->look;
    cam->stack[cam->stack_depth].yaw = cam->yaw;
    cam->stack_depth++;
    cam->mode = mode;
    /* Mode-specific fields start clean; the caller sets them after the push. */
    if (mode == KILN_CAM_TARGETING) cam->target_actor = KILN_ACTOR_HANDLE_NONE;
    return 0;
}

int kiln_camera_pop(KilnCamera *cam)
{
    if (cam->stack_depth <= 0) return -1;
    cam->stack_depth--;
    cam->mode = cam->stack[cam->stack_depth].mode;
    cam->target_actor = cam->stack[cam->stack_depth].target_actor;
    /* Restore the smoothed state so the camera doesn't swim back across the
     * level when the pushed mode ends. */
    cam->eye = cam->stack[cam->stack_depth].eye;
    cam->look = cam->stack[cam->stack_depth].look;
    cam->yaw = cam->stack[cam->stack_depth].yaw;
    return 0;
}

void kiln_camera_set_target_actor(KilnCamera *cam, KilnActorHandle h)
{
    cam->target_actor = h;
}

void kiln_camera_set_cutscene(KilnCamera *cam, fm_vec3_t eye, fm_vec3_t look)
{
    cam->cutscene_eye = eye;
    cam->cutscene_look = look;
}

void kiln_camera_set_collision(KilnCamera *cam, int enabled)
{
    cam->collision_enabled = enabled ? 1 : 0;
}