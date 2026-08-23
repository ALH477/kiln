/* SPDX-License-Identifier: MIT
 *
 * kiln_target.c — see kiln_target.h for the model.
 *
 * The math here is deliberately conservative: every vector op is one or two
 * multiplies/adds per axis, the cone test is one dot product, and the angle
 * sort is a linear walk with one compare per candidate. No sqrt in the hot
 * path (fm_vec3_len is only called on the final pick for tie-breaks and on
 * the acquire candidate for return).
 */

#include "kiln_target.h"
#include "kiln_gui.h"

#include <stddef.h>

typedef struct {
    KilnActorHandle h;
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

KilnActorHandle kiln_target_acquire(fm_vec3_t eye, fm_vec3_t fwd,
                                  float cone_half, float max_range)
{
    Cand best = { KILN_ACTOR_HANDLE_NONE, -2.0f, 0.0f };
    for (int cati = KILN_ACTOR_CAT_ENEMY; cati <= KILN_ACTOR_CAT_NPC; cati++) {
        for (KilnActor *a = kiln_actor_first((uint8_t)cati); a; a = kiln_actor_next(a)) {
            float score, r2;
            if (!in_cone(eye, fwd, cone_half, max_range, a->xform.pos, &score, &r2)) continue;
            if (score > best.score ||
                (score == best.score && r2 < best.range2)) {
                best.h = kiln_actor_handle_of(a);
                best.score = score;
                best.range2 = r2;
            }
        }
    }
    return best.h;
}

KilnActorHandle kiln_target_switch(KilnActorHandle cur, fm_vec3_t eye, fm_vec3_t fwd,
                                 fm_vec3_t cam_right, float cone_half, float max_range,
                                 fm_vec3_t dir)
{
    /* Direction bias: prefer candidates whose camera-space offset from the
     * current lock (or eye if no lock) aligns with `dir`. dir is in (right,
     * forward) space; convert each candidate's offset to that space and
     * dot with dir. */
    fm_vec3_t cur_pos;
    KilnActor *ca = kiln_actor_resolve(cur);
    cur_pos = ca ? ca->xform.pos : eye;

    Cand best = { KILN_ACTOR_HANDLE_NONE, -2.0f, 0.0f };
    for (int cati = KILN_ACTOR_CAT_ENEMY; cati <= KILN_ACTOR_CAT_NPC; cati++) {
        for (KilnActor *a = kiln_actor_first((uint8_t)cati); a; a = kiln_actor_next(a)) {
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
                best.h = kiln_actor_handle_of(a);
                best.score = combined;
                best.range2 = r2;
            }
        }
    }
    return best.h;
}

void kiln_target_draw_reticle(const KilnScene *scene, fm_vec3_t world,
                             int screen_w, int screen_h, color_t color)
{
    /* kiln_scene_project does the view-basis + perspective divide (this
     * function used to inline it; kiln_widget's board view needs the same
     * maths, so it lives in kiln_engine now). The reticle's own policy is to
     * clamp to the screen edge in BOTH the in-front-but-off-screen and the
     * behind-the-camera cases — a reticle that vanishes when the target
     * leaves the frame is the same information, harder to read. */
    int sx, sy;
    kiln_scene_project(scene, world, screen_w, screen_h, &sx, &sy);
    if (sx < 8) sx = 8; else if (sx > screen_w - 8) sx = screen_w - 8;
    if (sy < 8) sy = 8; else if (sy > screen_h - 8) sy = screen_h - 8;

    /* Four corner brackets, 12×12, 2 px thick (kiln_gui has no thick rect;
     * draw three 2-px-wide rects per corner). Keep it small so it reads as
     * a reticle, not a frame. */
    int s = 12, t = 2;
    kiln_gui_panel(sx - s, sy - s, t, s, color, color);              /* TL vertical */
    kiln_gui_panel(sx - s, sy - s, s, t, color, color);              /* TL horizontal */
    kiln_gui_panel(sx + s - t, sy - s, t, s, color, color);          /* TR vertical */
    kiln_gui_panel(sx,     sy - s, s, t, color, color);             /* TR horizontal */
    kiln_gui_panel(sx - s, sy + s - t, t, s, color, color);          /* BL vertical */
    kiln_gui_panel(sx - s, sy + s - t, s, t, color, color);          /* BL horizontal */
    kiln_gui_panel(sx + s - t, sy + s - t, t, s, color, color);      /* BR vertical */
    kiln_gui_panel(sx,     sy + s - t, s, t, color, color);          /* BR horizontal */
}