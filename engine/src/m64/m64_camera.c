/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_camera.c — see m64_camera.h for the model.
 */

#include "m64_camera.h"

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

void m64_camera_update(M64Camera *cam, fm_vec3_t target_pos, float target_yaw, float dt)
{
    /* The boom's own heading damps toward the target's facing first — the
     * eye position is then derived from the DAMPED yaw, not target_yaw
     * directly, so a snap turn swings the boom around the target over
     * several frames instead of instantly relocating the eye to the far
     * side of it. */
    cam->yaw = fm_lerp_angle(cam->yaw, target_yaw, damp_t(cam->yaw_speed, dt));

    fm_vec3_t desired_eye = boom_eye(target_pos, cam->yaw, cam->distance, cam->height);
    fm_vec3_t desired_look = (fm_vec3_t){ {
        target_pos.v[0], target_pos.v[1] + cam->look_height, target_pos.v[2],
    } };

    float t = damp_t(cam->pos_speed, dt);
    fm_vec3_lerp(&cam->eye, &cam->eye, &desired_eye, t);
    fm_vec3_lerp(&cam->look, &cam->look, &desired_look, t);
}

void m64_camera_apply(const M64Camera *cam, M64Scene *scene)
{
    scene->cam_pos = cam->eye;
    scene->cam_target = cam->look;
}
