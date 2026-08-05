/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_context.c — see m64_context.h for the model.
 */

#include "m64_context.h"

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

const char *m64_context_label(M64ContextAction action)
{
    if (action <= M64_CTX_PICKUP) return LABELS[action];
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

M64ContextAction m64_context_scan(fm_vec3_t player_pos, float yaw,
                                   float max_dist, float cone_half,
                                   M64ActorHandle *out_actor)
{
    if (out_actor) *out_actor = M64_ACTOR_HANDLE_NONE;

    static const uint8_t cats[] = {
        M64_ACTOR_CAT_NPC, M64_ACTOR_CAT_DOOR, M64_ACTOR_CAT_CHEST,
        M64_ACTOR_CAT_PROP, M64_ACTOR_CAT_ITEM,
    };
    static const M64ContextAction act_map[] = {
        M64_CTX_TALK, M64_CTX_OPEN, M64_CTX_OPEN_CHEST,
        M64_CTX_USE, M64_CTX_PICKUP,
    };

    M64ActorHandle best = M64_ACTOR_HANDLE_NONE;
    M64ContextAction best_act = M64_CTX_NONE;
    float best_dist = max_dist;

    for (int c = 0; c < (int)(sizeof(cats)/sizeof(cats[0])); c++) {
        for (M64Actor *a = m64_actor_first(cats[c]); a; a = m64_actor_next(a)) {
            fm_vec3_t d = {{ a->xform.pos.v[0] - player_pos.v[0],
                              a->xform.pos.v[1] - player_pos.v[1],
                              a->xform.pos.v[2] - player_pos.v[2] }};
            float dist = fm_vec3_len(&d);
            if (dist > best_dist) continue;
            if (!in_cone(player_pos, yaw, a->xform.pos, max_dist, cone_half)) continue;
            best_dist = dist;
            best = m64_actor_handle_of(a);
            best_act = act_map[c];
            if (c == M64_ACTOR_CAT_DOOR) {
                if (a->health == 0) best_act = M64_CTX_OPEN;
                else if (a->health > 0) best_act = M64_CTX_UNLOCK;
            }
        }
    }
    if (out_actor) *out_actor = best;
    return best_act;
}