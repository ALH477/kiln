/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_fpscam.c — see m64_fpscam.h for the model.
 */

#include "m64_fpscam.h"
#include "m64_clip.h"

#include <fmath.h>

void m64_fpscam_init(M64FpsCam *cam)
{
    cam->pos        = (fm_vec3_t){ { 0, 0, 0 } };
    cam->yaw        = 0.0f;
    cam->pitch      = 0.0f;
    cam->move_speed  = 80.0f;
    cam->run_speed   = 140.0f;
    cam->look_speed  = 0.05f;
    cam->eye_height  = 3.0f;
    cam->gravity     = 540.0f;
    cam->jump_speed  = 180.0f;
    cam->vy          = 0.0f;
    cam->on_ground   = 0;
    cam->last_surf   = 0;
    cam->mins       = (fm_vec3_t){ { -8, -8, -24 } };
    cam->maxs       = (fm_vec3_t){ {  8,  8,  24 } };
}

void m64_fpscam_snap(M64FpsCam *cam, fm_vec3_t pos, float yaw, float pitch)
{
    cam->pos   = pos;
    cam->yaw   = yaw;
    cam->pitch = pitch;
    cam->vy    = 0.0f;
}

fm_vec3_t m64_fpscam_forward(const M64FpsCam *cam)
{
    float cp = fm_cosf(cam->pitch);
    return (fm_vec3_t){ {
        fm_sinf(cam->yaw) * cp,
        fm_sinf(cam->pitch),
        fm_cosf(cam->yaw) * cp,
    } };
}

fm_vec3_t m64_fpscam_right(const M64FpsCam *cam)
{
    return (fm_vec3_t){ {
        fm_cosf(cam->yaw),
        0.0f,
        -fm_sinf(cam->yaw),
    } };
}

void m64_fpscam_update(M64FpsCam *cam, const M64Input *in, float dt)
{
    /* Look: C-stick X → yaw, C-stick Y → pitch. */
    cam->yaw   += in->cstick_x * cam->look_speed;
    cam->pitch += in->cstick_y * cam->look_speed;

    /* Clamp pitch to avoid gimbal-flip. */
    if (cam->pitch >  M64_FPSCAM_PITCH_LIMIT) cam->pitch =  M64_FPSCAM_PITCH_LIMIT;
    if (cam->pitch < -M64_FPSCAM_PITCH_LIMIT) cam->pitch = -M64_FPSCAM_PITCH_LIMIT;

    /* Wrap yaw to [-pi, pi] for numerical stability. */
    if (cam->yaw >  3.14159f) cam->yaw -= 6.28318f;
    if (cam->yaw < -3.14159f) cam->yaw += 6.28318f;

    /* Movement: main stick X = strafe, stick Y = forward/back.
     * Build the horizontal forward (pitch ignored for movement). */
    float fwd_x = fm_sinf(cam->yaw);
    float fwd_z = fm_cosf(cam->yaw);
    float rgt_x = fm_cosf(cam->yaw);
    float rgt_z = -fm_sinf(cam->yaw);

    /* Run: hold R for sprint speed. */
    float speed = (in->buttons & M64_BTN_R) ? cam->run_speed : cam->move_speed;

    float dx = (fwd_x * in->stick_y + rgt_x * in->stick_x) * speed * dt;
    float dz = (fwd_z * in->stick_y + rgt_z * in->stick_x) * speed * dt;

    /* Horizontal movement via m64_clip_slide. */
    fm_vec3_t hvel = { { dx, 0, dz } };
    cam->pos = m64_clip_slide(cam->pos, hvel, cam->mins, cam->maxs, 4);

    /* Jump: B button edge, only if on ground. */
    if ((in->edges & M64_BTN_B) && cam->on_ground) {
        cam->vy = cam->jump_speed;
        cam->on_ground = 0;
    }

    /* Vertical integration: gravity + floor collision. */
    if (cam->gravity > 0.0f) {
        cam->vy -= cam->gravity * dt;
        float dy = cam->vy * dt;
        fm_vec3_t vvel = { { 0, dy, 0 } };
        fm_vec3_t up_pos = { { cam->pos.v[0], cam->pos.v[1] + dy, cam->pos.v[2] } };
        fm_vec3_t new_pos = m64_clip_slide(up_pos, vvel, cam->mins, cam->maxs, 2);
        cam->pos.v[1] = new_pos.v[1];

        /* If we moved less than requested, we hit something. */
        if (cam->vy > 0.0f && new_pos.v[1] < up_pos.v[1]) {
            cam->vy = 0.0f;
        }
    }

    /* Ground probe. */
    M64Trace g = m64_clip_ground(cam->pos, cam->mins, cam->maxs);
    cam->on_ground = (g.fraction < 1.0f) ? 1 : 0;
    cam->last_surf = g.hitsurface;
    if (cam->on_ground && cam->vy < 0.0f) {
        cam->vy = 0.0f;
    }
}

void m64_fpscam_apply(const M64FpsCam *cam, M64Scene *scene)
{
    scene->cam_pos = cam->pos;
    fm_vec3_t fwd = m64_fpscam_forward(cam);
    scene->cam_target = (fm_vec3_t){ {
        cam->pos.v[0] + fwd.v[0] * 100.0f,
        cam->pos.v[1] + fwd.v[1] * 100.0f,
        cam->pos.v[2] + fwd.v[2] * 100.0f,
    } };
    scene->cam_up = (fm_vec3_t){ { 0, 1, 0 } };
}