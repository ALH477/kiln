/* SPDX-License-Identifier: MIT
 *
 * kiln_context.c — see kiln_context.h for the model.
 */

#include "kiln_context.h"

#include <fmath.h>
#include <stddef.h>

static const char *LABELS[] = {
    "",
    "Talk",
    "Open",
    "Unlock",
    "Open",
    "Use",
    "Pick up",
};

const char *fig_context_label(FigContextAction action)
{
    if (action <= FIG_CTX_PICKUP) return LABELS[action];
    return "";
}

static int in_cone(fm_vec3_t player_pos, float yaw,
                   fm_vec3_t actor_pos, float max_dist, float cone_half)
{
    fm_vec3_t d = {{ actor_pos.v[0] - player_pos.v[0],
                     actor_pos.v[1] - player_pos.v[1],
                     actor_pos.v[2] - player_pos.v[2] }};
    float dist = fm_vec3_len(&d);
    if (dist > max_dist) return 0;
    if (dist < 0.1f) return 1;
    float fwd_x = fm_sinf(yaw), fwd_z = fm_cosf(yaw);
    float dot = (d.v[0] * fwd_x + d.v[2] * fwd_z) / dist;
    return dot >= fm_cosf(cone_half);
}

FigContextAction fig_context_scan(fm_vec3_t player_pos, float yaw,
                                   float max_dist, float cone_half,
                                   FigActorHandle *out_actor)
{
    if (out_actor) *out_actor = FIG_ACTOR_HANDLE_NONE;

    static const uint8_t cats[] = {
        FIG_ACTOR_CAT_NPC, FIG_ACTOR_CAT_DOOR, FIG_ACTOR_CAT_CHEST,
        FIG_ACTOR_CAT_PROP, FIG_ACTOR_CAT_ITEM,
    };
    static const FigContextAction act_map[] = {
        FIG_CTX_TALK, FIG_CTX_OPEN, FIG_CTX_OPEN_CHEST,
        FIG_CTX_USE, FIG_CTX_PICKUP,
    };

    FigActorHandle best = FIG_ACTOR_HANDLE_NONE;
    FigContextAction best_act = FIG_CTX_NONE;
    float best_dist = max_dist;

    for (int c = 0; c < (int)(sizeof(cats)/sizeof(cats[0])); c++) {
        for (FigActor *a = fig_actor_first(cats[c]); a; a = fig_actor_next(a)) {
            fm_vec3_t d = {{ a->xform.pos.v[0] - player_pos.v[0],
                              a->xform.pos.v[1] - player_pos.v[1],
                              a->xform.pos.v[2] - player_pos.v[2] }};
            float dist = fm_vec3_len(&d);
            if (dist > best_dist) continue;
            if (!in_cone(player_pos, yaw, a->xform.pos, max_dist, cone_half)) continue;
            best_dist = dist;
            best = fig_actor_handle_of(a);
            best_act = act_map[c];
            /* cats[c], not c: `c` is an index into cats[] (0..4) and the
             * DOOR category is 6, so comparing the index meant this branch
             * never ran and every door reported OPEN, locked or not. */
            if (cats[c] == FIG_ACTOR_CAT_DOOR) {
                if (a->health == 0) best_act = FIG_CTX_OPEN;
                else if (a->health > 0) best_act = FIG_CTX_UNLOCK;
            }
        }
    }
    if (out_actor) *out_actor = best;
    return best_act;
}