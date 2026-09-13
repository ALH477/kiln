/* SPDX-License-Identifier: MIT
 *
 * kiln_player.c — see kiln_player.h for the model.
 *
 * The state machine is a switch on KilnPlayerState, with transitions
 * guarded by input edges (kiln_input_pressed) and world state (ground
 * probe). Movement is camera-relative when a basis is set, world-relative
 * otherwise — the demo in examples/oot-demo sets the basis each frame
 * from the active camera mode.
 *
 * No sqrt in the hot path except one: stick magnitude is squared for the
 * walk/run threshold, jump/roll velocities are constants, and footstep
 * cadence is a simple inverse of horizontal speed. The single sqrtf in
 * the grounded locomotion branch (to normalise the wish direction) is
 * one fsqrt.s under -ffast-math — 29 cycles, cheaper than the fm_vec3_len
 * in the ground-probe normalisation that kiln_clip already does internally.
 */

#include "kiln_player.h"
#include "kiln_clip.h"
#include "kiln_event.h"
#include "kiln_input.h"

#include <fmath.h>
#include <stddef.h>
#include <string.h>

const fm_vec3_t KILN_PLAYER_MINS = {{ -8, -16, -8 }};
const fm_vec3_t KILN_PLAYER_MAXS = {{  8,  16,  8 }};

/* Module-static camera basis; default world axes. The caller sets this
 * each frame before kiln_player_update via kiln_player_set_camera_basis. */
static fm_vec3_t g_cam_fwd  = {{ 0, 0, 1 }};
static fm_vec3_t g_cam_right = {{ 1, 0, 0 }};
static int       g_cam_basis_set = 0;

void kiln_player_set_camera_basis(fm_vec3_t fwd, fm_vec3_t right)
{
    g_cam_fwd = fwd;
    g_cam_right = right;
    g_cam_basis_set = 1;
}

static const float WALK_THRESH = 0.20f;
static const float RUN_THRESH  = 0.85f;
static const float WALK_SPEED  = 60.0f;   /* world units / sec */
static const float RUN_SPEED  = 140.0f;
static const float ROLL_SPEED  = 200.0f;
static const float ROLL_TIME   = 0.45f;
static const float ATTACK_TIME = 0.4f;
static const float JUMP_SPEED  = 180.0f;   /* initial vy                       */
static const float GRAVITY     = 540.0f;   /* units / sec^2                    */

static void enter_state(KilnPlayer *p, KilnPlayerState s)
{
    p->state = s;
    p->state_t = 0.0f;
}

void kiln_player_update(KilnActor *self, int port, float dt)
{
    KilnPlayer *p = kiln_player_of(self);
    const KilnInput *in = kiln_input_get(port);

    p->state_t += dt;

    /* Horizontal wish direction in world space, camera-relative. */
    float sx = in->stick_x, sy = in->stick_y;
    int grounded = p->on_ground;

    /* Reusable horizontal wish basis. */
    fm_vec3_t fwd  = g_cam_fwd;
    fm_vec3_t right = g_cam_right;
    fwd.v[1] = 0; right.v[1] = 0;
    fm_vec3_norm(&fwd, &fwd);
    fm_vec3_norm(&right, &right);

    /* Handle transient states first; they lock the wish vel. */
    if (p->state == KILN_PLAYER_ROLL) {
        /* Roll: locked forward lunge; ignore stick. */
        fm_vec3_t roll_dir = {{ fm_sinf(p->yaw), 0, fm_cosf(p->yaw) }};
        fm_vec3_t vel = {{ roll_dir.v[0] * ROLL_SPEED * dt, p->vel.v[1] * dt,
                            roll_dir.v[2] * ROLL_SPEED * dt }};
        self->xform.pos = kiln_clip_slide(self->xform.pos, vel,
                                          KILN_PLAYER_MINS, KILN_PLAYER_MAXS, 4);
        if (p->state_t >= ROLL_TIME) enter_state(p, KILN_PLAYER_IDLE);
    } else if (p->state == KILN_PLAYER_ATTACK) {
        /* Attack: stand still, brief stop. */
        if (p->state_t >= ATTACK_TIME) enter_state(p, KILN_PLAYER_IDLE);
    } else if (p->state == KILN_PLAYER_JUMP || p->state == KILN_PLAYER_FALL) {
        /* Airborne: integrate gravity, allow small air control. */
        p->vel.v[1] -= GRAVITY * dt;
        float air = 0.4f; /* air-control fraction */
        fm_vec3_t wish = {{
            (fwd.v[0] * sy + right.v[0] * sx) * RUN_SPEED * air * dt,
            0,
            (fwd.v[2] * sy + right.v[2] * sx) * RUN_SPEED * air * dt,
        }};
        fm_vec3_t vel = {{ wish.v[0], p->vel.v[1] * dt, wish.v[2] }};
        self->xform.pos = kiln_clip_slide(self->xform.pos, vel,
                                          KILN_PLAYER_MINS, KILN_PLAYER_MAXS, 4);
        if (p->vel.v[1] < 0) p->state = KILN_PLAYER_FALL;
    }

    /* Transitions out of grounded states. */
    if (p->state == KILN_PLAYER_IDLE || p->state == KILN_PLAYER_WALK ||
        p->state == KILN_PLAYER_RUN) {
        if (kiln_input_pressed(port, KILN_BTN_L)) {
            enter_state(p, KILN_PLAYER_ROLL);
            return;
        }
        if (kiln_input_pressed(port, KILN_BTN_A)) {
            enter_state(p, KILN_PLAYER_ATTACK);
            return;
        }
        if (kiln_input_pressed(port, KILN_BTN_B) && grounded) {
            p->vel.v[1] = JUMP_SPEED;
            enter_state(p, KILN_PLAYER_JUMP);
            grounded = 0;
        }
    }

    /* Grounded locomotion. */
    if (p->state == KILN_PLAYER_IDLE || p->state == KILN_PLAYER_WALK ||
        p->state == KILN_PLAYER_RUN) {
        fm_vec3_t wish = {{
            (fwd.v[0] * sy + right.v[0] * sx),
            0,
            (fwd.v[2] * sy + right.v[2] * sx),
        }};
        float wlen2 = wish.v[0] * wish.v[0] + wish.v[2] * wish.v[2];
        int moving = wlen2 > WALK_THRESH * WALK_THRESH;
        int running = (in->buttons & KILN_BTN_R) || wlen2 > RUN_THRESH * RUN_THRESH;

        if (!moving) {
            enter_state(p, KILN_PLAYER_IDLE);
        } else {
            /* Normalise wish horizontal and scale by speed. */
            float wlen = sqrtf(wlen2);
            float nx = wish.v[0] / wlen, nz = wish.v[2] / wlen;
            float speed = running ? RUN_SPEED : WALK_SPEED;
            fm_vec3_t vel = {{ nx * speed * dt, 0, nz * speed * dt }};
            self->xform.pos = kiln_clip_slide(self->xform.pos, vel,
                                              KILN_PLAYER_MINS, KILN_PLAYER_MAXS, 4);
            /* Face the movement direction (instant for now; a damped turn
             * belongs in kiln_camera, not here). */
            p->yaw = fm_atan2f(nx, nz);
            enter_state(p, running ? KILN_PLAYER_RUN : KILN_PLAYER_WALK);
        }
    }

    /* Vertical integration if airborne. (Done in the airborne branch
     * above; here we just apply any leftover gravity for grounded states
     * to keep vy consistent on a walk-off-ledge transition.) */
    if (p->state != KILN_PLAYER_JUMP && p->state != KILN_PLAYER_FALL) {
        /* If the ground probe loses contact, transition to FALL. */
        if (grounded) p->vel.v[1] = 0;
    }

    /* Ground probe. Done every frame, regardless of state, so on_ground
     * and the surface id stay current for footsteps and landing. */
    KilnTrace g = kiln_clip_ground(self->xform.pos, KILN_PLAYER_MINS, KILN_PLAYER_MAXS);
    p->on_ground = (g.fraction < 1.0f) ? 1 : 0;
    p->last_surf = g.hitsurface;
    if (p->on_ground && (p->state == KILN_PLAYER_FALL)) {
        enter_state(p, KILN_PLAYER_IDLE);
        p->vel.v[1] = 0;
    }
    if (!p->on_ground && p->state != KILN_PLAYER_JUMP &&
        p->state != KILN_PLAYER_FALL && p->state != KILN_PLAYER_ROLL &&
        p->state != KILN_PLAYER_ATTACK) {
        enter_state(p, KILN_PLAYER_FALL);
    }

    /* Update the actor's yaw on the transform so the renderer sees the
     * facing, for a model whose front is +Z. NEGATED, because the two angles
     * turn opposite ways: `yaw` is fm_atan2f(x, z), so it faces
     * (sin yaw, cos yaw), while libdragon's fm_mat4_from_axis_angle about +Y
     * maps +Z to (-sin a, 0, cos a) — measured natively. Writing yaw here drew
     * the player mirrored whenever it moved off the Z axis. A model facing -Z
     * wants PI - yaw, which the caller sets after this. */
    self->xform.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
    self->xform.rot_angle = -p->yaw;

    /* Footstep cadence while walking/running on ground. Cadence is
     * inversely proportional to horizontal speed: walk ~2 Hz, run ~4 Hz. */
    if (p->on_ground &&
        (p->state == KILN_PLAYER_WALK || p->state == KILN_PLAYER_RUN)) {
        p->step_cd -= dt;
        if (p->step_cd <= 0.0f) {
            int32_t args[1] = { (int32_t)p->last_surf };
            kiln_event_post(kiln_actor_handle_of(self), KILN_EV_PLAYER_FOOTSTEP,
                            0 /* fire immediately */, args, 1, 1);
            p->step_cd = (p->state == KILN_PLAYER_RUN) ? 0.25f : 0.5f;
        }
    } else {
        p->step_cd = 0.0f;
    }
}