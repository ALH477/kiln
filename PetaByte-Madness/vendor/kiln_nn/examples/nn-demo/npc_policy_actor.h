/* SPDX-License-Identifier: MPL-2.0 */
/**
 * Example sketch: how a Kiln actor can own and drive a kiln_nn policy.
 *
 * This is intentionally minimal and does not depend on the full engine
 * headers. Wire the same pattern into your real KilnActorProfile.
 */

#ifndef NPC_POLICY_ACTOR_H
#define NPC_POLICY_ACTOR_H

#include "kiln_nn.h"

/* Example discrete actions for a simple chase / combat NPC */
enum {
    NPC_ACT_IDLE = 0,
    NPC_ACT_CHASE,
    NPC_ACT_FLEE,
    NPC_ACT_ATTACK,
    NPC_ACT_TAUNT,
    NPC_ACT_COUNT
};

typedef struct {
    KilnNnModel policy;
    uint8_t    scratch[4096];   /* size with kiln_nn_scratch_bytes() */
    int        last_action;
} NpcPolicyState;

/**
 * Initialise the policy from a generated layer table
 * (see tools/nn/train_export.py output).
 */
static inline bool npc_policy_init(NpcPolicyState *st,
                                    const KilnNnLayer *layers,
                                    uint8_t n_layers,
                                    uint16_t obs_dim,
                                    uint16_t action_dim)
{
    return kiln_nn_init(&st->policy,
                       KILN_NN_DTYPE_INT8,
                       n_layers, layers,
                       obs_dim, action_dim,
                       NULL,
                       st->scratch, sizeof(st->scratch));
}

/**
 * Decide an action from a pre-built observation vector.
 *
 * The game is responsible for filling obs_f[] from actor state,
 * distances, visibility, health, Z-target flags, room id, etc.
 * Values should be roughly in [-1, 1].
 */
static inline int npc_policy_decide(NpcPolicyState *st,
                                     const float *obs_f)
{
    int8_t obs_i8[KILN_NN_MAX_OBS];
    kiln_nn_pack_obs_i8(obs_i8, st->policy.obs_dim, obs_f);

    int action = kiln_nn_argmax(&st->policy, obs_i8);
    st->last_action = action;
    return action;
}

#endif /* NPC_POLICY_ACTOR_H */
