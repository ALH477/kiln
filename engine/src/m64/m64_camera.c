/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_camera.c — see m64_camera.h for the model.
 */

#include "m64_camera.h"
#include "m64_clip.h"

#include <fmath.h>

static float damp_t(float speed, float dt)
{
    float t = speed * dt;
    return t < 1.0f ? t : 1.0f;
}

void m64_camera_init(M64Camera *cam)
{
    cam->distance = 6.0f;
    cam->height = 3.0f;
    cam->look_height = 1.5f;

    cam->pos_speed = 10.0f;
    cam->yaw_speed = 4.0f;

    cam->yaw = 0.0f;
    cam->eye = (fm_vec3_t){ { 0, 0, 0 } };
    cam->look = (fm_vec3_t){ { 0, 0, 0 } };

    cam->mode = M64_CAM_NORMAL;
    cam->collision_enabled = 0;
    cam->target_actor = M64_ACTOR_HANDLE_NONE;
    cam->cutscene_eye  = (fm_vec3_t){ { 0, 0, 0 } };
    cam->cutscene_look = (fm_vec3_t){ { 0, 0, 0 } };
    cam->stack_depth = 0;
}

/** Boom position for a given target position + heading: `distance` behind
 *  the heading (heading 0 == +Z, matching M64Actor's yaw), `height` up. */
static fm_vec3_t boom_eye(fm_vec3_t target_pos, float yaw, float distance, float height)
{
    return (fm_vec3_t){ {
        target_pos.v[0] - fm_sinf(yaw) * distance,
        target_pos.v[1] + height,
        target_pos.v[2] - fm_cosf(yaw) * distance,
    } };
}

void m64_camera_snap(M64Camera *cam, fm_vec3_t target_pos, float target_yaw)
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
    M64Trace tr = m64_clip_ray(look, desired_eye);
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

static void update_normal(M64Camera *cam, fm_vec3_t target_pos, float target_yaw, float dt)
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

static void update_targeting(M64Camera *cam, fm_vec3_t target_pos, float dt)
{
    /* Sit the eye on the far side of the targeter from the locked actor,
     * so both are in frame. Look at the midpoint of the two. A shorter
     * boom than NORMAL keeps the framing tight. */
    M64Actor *t = m64_actor_resolve(cam->target_actor);
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

static void update_cutscene(M64Camera *cam, float dt)
{
    fm_vec3_t desired_eye  = cam->cutscene_eye;
    fm_vec3_t desired_look = cam->cutscene_look;
    if (cam->collision_enabled) desired_eye = collide_boom(desired_look, desired_eye);
    float t = damp_t(cam->pos_speed, dt);
    fm_vec3_lerp(&cam->eye, &cam->eye, &desired_eye, t);
    fm_vec3_lerp(&cam->look, &cam->look, &desired_look, t);
}

void m64_camera_update(M64Camera *cam, fm_vec3_t target_pos, float target_yaw, float dt)
{
    switch (cam->mode) {
    case M64_CAM_TARGETING: update_targeting(cam, target_pos, dt); break;
    case M64_CAM_CUTSCENE:  update_cutscene(cam, dt); break;
    case M64_CAM_NORMAL:
    default:                update_normal(cam, target_pos, target_yaw, dt); break;
    }
}

void m64_camera_apply(const M64Camera *cam, M64Scene *scene)
{
    scene->cam_pos = cam->eye;
    scene->cam_target = cam->look;
}

int m64_camera_push(M64Camera *cam, M64CamMode mode)
{
    if (cam->stack_depth >= M64_CAM_STACK_DEPTH) return -1;
    cam->stack[cam->stack_depth].mode = cam->mode;
    cam->stack[cam->stack_depth].target_actor = cam->target_actor;
    cam->stack[cam->stack_depth].eye = cam->eye;
    cam->stack[cam->stack_depth].look = cam->look;
    cam->stack[cam->stack_depth].yaw = cam->yaw;
    cam->stack_depth++;
    cam->mode = mode;
    /* Mode-specific fields start clean; the caller sets them after the push. */
    if (mode == M64_CAM_TARGETING) cam->target_actor = M64_ACTOR_HANDLE_NONE;
    return 0;
}

int m64_camera_pop(M64Camera *cam)
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

void m64_camera_set_target_actor(M64Camera *cam, M64ActorHandle h)
{
    cam->target_actor = h;
}

void m64_camera_set_cutscene(M64Camera *cam, fm_vec3_t eye, fm_vec3_t look)
{
    cam->cutscene_eye = eye;
    cam->cutscene_look = look;
}

void m64_camera_set_collision(M64Camera *cam, int enabled)
{
    cam->collision_enabled = enabled ? 1 : 0;
}