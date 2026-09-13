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

const char *kiln_context_label(KilnContextAction action)
{
    if (action <= KILN_CTX_PICKUP) return LABELS[action];
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

KilnContextAction kiln_context_scan(fm_vec3_t player_pos, float yaw,
                                   float max_dist, float cone_half,
                                   KilnActorHandle *out_actor)
{
    if (out_actor) *out_actor = KILN_ACTOR_HANDLE_NONE;

    static const uint8_t cats[] = {
        KILN_ACTOR_CAT_NPC, KILN_ACTOR_CAT_DOOR, KILN_ACTOR_CAT_CHEST,
        KILN_ACTOR_CAT_PROP, KILN_ACTOR_CAT_ITEM,
    };
    static const KilnContextAction act_map[] = {
        KILN_CTX_TALK, KILN_CTX_OPEN, KILN_CTX_OPEN_CHEST,
        KILN_CTX_USE, KILN_CTX_PICKUP,
    };

    KilnActorHandle best = KILN_ACTOR_HANDLE_NONE;
    KilnContextAction best_act = KILN_CTX_NONE;
    float best_dist = max_dist;

    for (int c = 0; c < (int)(sizeof(cats)/sizeof(cats[0])); c++) {
        for (KilnActor *a = kiln_actor_first(cats[c]); a; a = kiln_actor_next(a)) {
            fm_vec3_t d = {{ a->xform.pos.v[0] - player_pos.v[0],
                              a->xform.pos.v[1] - player_pos.v[1],
                              a->xform.pos.v[2] - player_pos.v[2] }};
            float dist = fm_vec3_len(&d);
            if (dist > best_dist) continue;
            if (!in_cone(player_pos, yaw, a->xform.pos, max_dist, cone_half)) continue;
            best_dist = dist;
            best = kiln_actor_handle_of(a);
            best_act = act_map[c];
            /* cats[c], not c: `c` is an index into cats[] (0..4) and the
             * DOOR category is 6, so comparing the index meant this branch
             * never ran and every door reported OPEN, locked or not. */
            if (cats[c] == KILN_ACTOR_CAT_DOOR) {
                if (a->health == 0) best_act = KILN_CTX_OPEN;
                else if (a->health > 0) best_act = KILN_CTX_UNLOCK;
            }
        }
    }
    if (out_actor) *out_actor = best;
    return best_act;
}