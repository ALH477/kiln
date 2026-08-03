/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_target.c — see m64_target.h for the model.
 *
 * The math here is deliberately conservative: every vector op is one or two
 * multiplies/adds per axis, the cone test is one dot product, and the angle
 * sort is a linear walk with one compare per candidate. No sqrt in the hot
 * path (fm_vec3_len is only called on the final pick for tie-breaks and on
 * the acquire candidate for return).
 */

#include "m64_target.h"
#include "m64_gui.h"

#include <stddef.h>

typedef struct {
    M64ActorHandle h;
    float score;   /* dot(fwd, dir_to_cand); higher = more centred */
    float range2;  /* squared range, for tie-breaks */
} Cand;

static int in_cone(fm_vec3_t eye, fm_vec3_t fwd, float cone_half, float max_range,
                    fm_vec3_t cand, float *score_out, float *range2_out)
{
    fm_vec3_t d = {{ cand.v[0] - eye.v[0],
                     cand.v[1] - eye.v[1],
                     cand.v[2] - eye.v[2] }};
    float r2 = d.v[0] * d.v[0] + d.v[1] * d.v[1] + d.v[2] * d.v[2];
    if (r2 > max_range * max_range) return 0;
    /* fm_vec3_len is sqrtf under the hood; on MIPS with -ffast-math that's
     * one instruction (fsqrt.s), same as a divide. */
    float r = fm_vec3_len(&d);
    if (r < 1e-3f) return 0;
    float dot = (fwd.v[0] * d.v[0] + fwd.v[1] * d.v[1] + fwd.v[2] * d.v[2]) / r;
    if (dot < fm_cosf(cone_half)) return 0;
    if (score_out)  *score_out  = dot;
    if (range2_out) *range2_out = r2;
    return 1;
}

M64ActorHandle m64_target_acquire(fm_vec3_t eye, fm_vec3_t fwd,
                                  float cone_half, float max_range)
{
    Cand best = { M64_ACTOR_HANDLE_NONE, -2.0f, 0.0f };
    for (int cati = M64_ACTOR_CAT_ENEMY; cati <= M64_ACTOR_CAT_NPC; cati++) {
        for (M64Actor *a = m64_actor_first((uint8_t)cati); a; a = m64_actor_next(a)) {
            float score, r2;
            if (!in_cone(eye, fwd, cone_half, max_range, a->xform.pos, &score, &r2)) continue;
            if (score > best.score ||
                (score == best.score && r2 < best.range2)) {
                best.h = m64_actor_handle_of(a);
                best.score = score;
                best.range2 = r2;
            }
        }
    }
    return best.h;
}

M64ActorHandle m64_target_switch(M64ActorHandle cur, fm_vec3_t eye, fm_vec3_t fwd,
                                 fm_vec3_t cam_right, float cone_half, float max_range,
                                 fm_vec3_t dir)
{
    /* Direction bias: prefer candidates whose camera-space offset from the
     * current lock (or eye if no lock) aligns with `dir`. dir is in (right,
     * forward) space; convert each candidate's offset to that space and
     * dot with dir. */
    fm_vec3_t cur_pos;
    M64Actor *ca = m64_actor_resolve(cur);
    cur_pos = ca ? ca->xform.pos : eye;

    Cand best = { M64_ACTOR_HANDLE_NONE, -2.0f, 0.0f };
    for (int cati = M64_ACTOR_CAT_ENEMY; cati <= M64_ACTOR_CAT_NPC; cati++) {
        for (M64Actor *a = m64_actor_first((uint8_t)cati); a; a = m64_actor_next(a)) {
            if (a == ca) continue;
            float score, r2;
            if (!in_cone(eye, fwd, cone_half, max_range, a->xform.pos, &score, &r2)) continue;

            fm_vec3_t off = {{ a->xform.pos.v[0] - cur_pos.v[0],
                               a->xform.pos.v[1] - cur_pos.v[1],
                               a->xform.pos.v[2] - cur_pos.v[2] }};
            float d_right = (off.v[0] * cam_right.v[0] +
                              off.v[1] * cam_right.v[1] +
                              off.v[2] * cam_right.v[2]);
            float d_fwd   = (off.v[0] * fwd.v[0] +
                              off.v[1] * fwd.v[1] +
                              off.v[2] * fwd.v[2]);
            /* bias = how aligned (off in right/fwd space) is with dir */
            float bias = d_right * dir.v[0] + d_fwd * dir.v[2];
            /* Combined score weights centring in the cone AND the switch
             * direction; the constants are tuned so a stick-tap dominates
             * centring without throwing the cone out entirely. */
            float combined = bias * 1.0f + score * 0.3f;
            if (combined > best.score) {
                best.h = m64_actor_handle_of(a);
                best.score = combined;
                best.range2 = r2;
            }
        }
    }
    return best.h;
}

void m64_target_draw_reticle(const M64Scene *scene, fm_vec3_t world,
                             int screen_w, int screen_h, color_t color)
{
    /* Build a view basis from the scene's camera fields. */
    fm_vec3_t fwd = {{ scene->cam_target.v[0] - scene->cam_pos.v[0],
                       scene->cam_target.v[1] - scene->cam_pos.v[1],
                       scene->cam_target.v[2] - scene->cam_pos.v[2] }};
    fm_vec3_norm(&fwd, &fwd);
    fm_vec3_t up = scene->cam_up;
    fm_vec3_t right;
    fm_vec3_cross(&right, &fwd, &up);
    fm_vec3_norm(&right, &right);
    fm_vec3_t real_up;
    fm_vec3_cross(&real_up, &right, &fwd);

    fm_vec3_t d = {{ world.v[0] - scene->cam_pos.v[0],
                     world.v[1] - scene->cam_pos.v[1],
                     world.v[2] - scene->cam_pos.v[2] }};
    float vz = d.v[0] * fwd.v[0] + d.v[1] * fwd.v[1] + d.v[2] * fwd.v[2];
    float vx = d.v[0] * right.v[0] + d.v[1] * right.v[1] + d.v[2] * right.v[2];
    float vy = d.v[0] * real_up.v[0] + d.v[1] * real_up.v[1] + d.v[2] * real_up.v[2];

    /* Behind the camera: clamp to the nearer edge. */
    int sx, sy;
    if (vz <= 0.001f) {
        /* Project to the screen edge in the direction of vx/vy. */
        float inv = 1.0f / (vz < 0 ? -1e-3f : 1e-3f);
        float nx = vx * inv;
        float ny = vy * inv;
        sx = (int)((nx + 1.0f) * 0.5f * screen_w);
        sy = (int)((1.0f - ny) * 0.5f * screen_h);
        if (sx < 8) sx = 8; else if (sx > screen_w - 8) sx = screen_w - 8;
        if (sy < 8) sy = 8; else if (sy > screen_h - 8) sy = screen_h - 8;
    } else {
        float aspect = (float)screen_w / (float)screen_h;
        float fov_rad = T3D_DEG_TO_RAD(scene->fov_deg);
        /* fmath has no tanf (it would be a libm call); sinf/cosf are inlined,
         * so tan = sin/cos costs two multiplies and a divide, no libm. */
        float half = fov_rad * 0.5f;
        float tan_half_y = fm_sinf(half) / fm_cosf(half);
        float tan_half_x = tan_half_y * aspect;
        float nx = (vx / vz) / tan_half_x;
        float ny = (vy / vz) / tan_half_y;
        sx = (int)((nx + 1.0f) * 0.5f * screen_w);
        sy = (int)((1.0f - ny) * 0.5f * screen_h);
        /* Off-screen in front: clamp to edge so the reticle is still visible. */
        if (sx < 8) sx = 8; else if (sx > screen_w - 8) sx = screen_w - 8;
        if (sy < 8) sy = 8; else if (sy > screen_h - 8) sy = screen_h - 8;
    }

    /* Four corner brackets, 12×12, 2 px thick (m64_gui has no thick rect;
     * draw three 2-px-wide rects per corner). Keep it small so it reads as
     * a reticle, not a frame. */
    int s = 12, t = 2;
    m64_gui_panel(sx - s, sy - s, t, s, color, color);              /* TL vertical */
    m64_gui_panel(sx - s, sy - s, s, t, color, color);              /* TL horizontal */
    m64_gui_panel(sx + s - t, sy - s, t, s, color, color);          /* TR vertical */
    m64_gui_panel(sx,     sy - s, s, t, color, color);             /* TR horizontal */
    m64_gui_panel(sx - s, sy + s - t, t, s, color, color);          /* BL vertical */
    m64_gui_panel(sx - s, sy + s - t, s, t, color, color);          /* BL horizontal */
    m64_gui_panel(sx + s - t, sy + s - t, t, s, color, color);      /* BR vertical */
    m64_gui_panel(sx,     sy + s - t, s, t, color, color);          /* BR horizontal */
}