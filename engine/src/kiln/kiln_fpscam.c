/* SPDX-License-Identifier: MIT
 *
 * kiln_fpscam.c — see kiln_fpscam.h for the model.
 */

#include "kiln_fpscam.h"
#include "kiln_clip.h"

#include <fmath.h>

void kiln_fpscam_init(KilnFpsCam *cam)
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
    /* The bindings this module used to hardcode. Defaulted here so every
     * existing caller is byte-for-byte unchanged, and overridable so a game
     * that needs R and B for something else can say so. */
    cam->btn_run    = KILN_BTN_R;
    cam->btn_jump   = KILN_BTN_B;
}

void kiln_fpscam_snap(KilnFpsCam *cam, fm_vec3_t pos, float yaw, float pitch)
{
    cam->pos   = pos;
    cam->yaw   = yaw;
    cam->pitch = pitch;
    cam->vy    = 0.0f;
}

fm_vec3_t kiln_fpscam_forward(const KilnFpsCam *cam)
{
    float cp = fm_cosf(cam->pitch);
    return (fm_vec3_t){ {
        fm_sinf(cam->yaw) * cp,
        fm_sinf(cam->pitch),
        fm_cosf(cam->yaw) * cp,
    } };
}

/* ── Handedness ──────────────────────────────────────────────────────
 * The renderer is right-handed: t3d_viewport_look_at builds side = forward x
 * up, so an eye looking down +Z has screen-right at -X. Right is therefore
 * forward x up = (-cos yaw, 0, sin yaw), and turning right swings forward
 * toward it, which DECREASES yaw.
 *
 * This used to return (cos, 0, -sin) and add C-stick X to yaw — screen-left
 * both times, so stick right strafed left and C right turned left in every
 * consumer, on console as well as on the host. nix/checks/kiln-fpscam.nix asks
 * the look-at matrix where screen-right is and holds the pad to it. */
fm_vec3_t kiln_fpscam_right(const KilnFpsCam *cam)
{
    return (fm_vec3_t){ {
        -fm_cosf(cam->yaw),
        0.0f,
        fm_sinf(cam->yaw),
    } };
}

void kiln_fpscam_update(KilnFpsCam *cam, const KilnInput *in, float dt)
{
    /* Look: C-stick X → yaw, C-stick Y → pitch. Right is negative yaw; see
     * kiln_fpscam_right. */
    cam->yaw   -= in->cstick_x * cam->look_speed;
    cam->pitch += in->cstick_y * cam->look_speed;

    /* Clamp pitch to avoid gimbal-flip. */
    if (cam->pitch >  KILN_FPSCAM_PITCH_LIMIT) cam->pitch =  KILN_FPSCAM_PITCH_LIMIT;
    if (cam->pitch < -KILN_FPSCAM_PITCH_LIMIT) cam->pitch = -KILN_FPSCAM_PITCH_LIMIT;

    /* Wrap yaw to [-pi, pi] for numerical stability. */
    if (cam->yaw >  3.14159f) cam->yaw -= 6.28318f;
    if (cam->yaw < -3.14159f) cam->yaw += 6.28318f;

    /* Movement: main stick X = strafe, stick Y = forward/back.
     * Build the horizontal forward (pitch ignored for movement). */
    float fwd_x = fm_sinf(cam->yaw);
    float fwd_z = fm_cosf(cam->yaw);
    float rgt_x = -fm_cosf(cam->yaw);
    float rgt_z = fm_sinf(cam->yaw);

    /* Run: hold R for sprint speed. */
    /* A zero mask disables the action: `x & 0` is never true, so a game that
     * sets btn_run = 0 simply has no sprint, with no branch of its own. */
    float speed = (cam->btn_run && (in->buttons & cam->btn_run))
                      ? cam->run_speed : cam->move_speed;

    float dx = (fwd_x * in->stick_y + rgt_x * in->stick_x) * speed * dt;
    float dz = (fwd_z * in->stick_y + rgt_z * in->stick_x) * speed * dt;

    /* Horizontal movement via kiln_clip_slide. */
    fm_vec3_t hvel = { { dx, 0, dz } };
    cam->pos = kiln_clip_slide(cam->pos, hvel, cam->mins, cam->maxs, 4);

    /* Jump: B button edge, only if on ground. */
    if (cam->btn_jump && (in->edges & cam->btn_jump) && cam->on_ground) {
        cam->vy = cam->jump_speed;
        cam->on_ground = 0;
    }

    /* Vertical integration: gravity + floor collision.
     *
     * ── The sweep starts at cam->pos, NOT at cam->pos + dy ──────────────
     * kiln_clip_slide(pos, vel, ...) takes a START position and a full-frame
     * DISPLACEMENT and returns where the box ends up (see kiln_clip.h). This
     * used to pre-add dy to the start position and then hand the same dy in as
     * the displacement, which did two wrong things at once:
     *
     *   1. Moved 2*dy per frame, so the fall accelerated at twice gravity.
     *   2. Far worse — began the sweep BELOW the floor it was falling onto.
     *      A swept AABB starting already past a surface has nothing to hit, so
     *      the floor was skipped entirely and the player kept going. Where they
     *      finally stopped was wherever kiln_clip_ground's short probe happened
     *      to catch a brush's underside.
     *
     * In real use that put the player about 111 units — 1.7 m — under the
     * floor for the whole playable section, with the room's contents
     * squeezed into the top of the frame and a large dark nothing below. It
     * read as a lighting or camera-framing problem, and a known "the player
     * can leave the room mid-fall" comment in the game's own code had been
     * treated as a fact of life rather than a symptom. Found in one capture
     * once kiln_debugdraw could draw the room's brushes next to the camera the
     * scene was actually built from. */
    if (cam->gravity > 0.0f) {
        cam->vy -= cam->gravity * dt;
        const float dy = cam->vy * dt;
        const fm_vec3_t vvel = { { 0, dy, 0 } };
        const float want_y = cam->pos.v[1] + dy;
        fm_vec3_t new_pos = kiln_clip_slide(cam->pos, vvel,
                                           cam->mins, cam->maxs, 2);
        cam->pos.v[1] = new_pos.v[1];

        /* Blocked: we did not travel the full dy, so kill the vertical speed
         * rather than letting it build against a surface. Compared against the
         * REQUESTED destination — comparing against the (already displaced)
         * start position, as this did, meant a rising cam could never detect a
         * ceiling and a falling one could never detect a floor. */
        if ((dy > 0.0f && new_pos.v[1] < want_y - 1e-3f) ||
            (dy < 0.0f && new_pos.v[1] > want_y + 1e-3f)) {
            cam->vy = 0.0f;
        }
    }

    /* Ground probe. */
    KilnTrace g = kiln_clip_ground(cam->pos, cam->mins, cam->maxs);
    cam->on_ground = (g.fraction < 1.0f) ? 1 : 0;
    cam->last_surf = g.hitsurface;
    if (cam->on_ground && cam->vy < 0.0f) {
        cam->vy = 0.0f;
    }
}

void kiln_fpscam_apply(const KilnFpsCam *cam, KilnScene *scene)
{
    scene->cam_pos = cam->pos;
    fm_vec3_t fwd = kiln_fpscam_forward(cam);
    scene->cam_target = (fm_vec3_t){ {
        cam->pos.v[0] + fwd.v[0] * 100.0f,
        cam->pos.v[1] + fwd.v[1] * 100.0f,
        cam->pos.v[2] + fwd.v[2] * 100.0f,
    } };
    scene->cam_up = (fm_vec3_t){ { 0, 1, 0 } };
}