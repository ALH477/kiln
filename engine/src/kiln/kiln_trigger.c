/* SPDX-License-Identifier: MIT
 *
 * kiln_trigger.c — see kiln_trigger.h for the model.
 */

#include "kiln_trigger.h"
#include "kiln_event.h"

#include <stddef.h>
#include <string.h>

static KilnTrigger g_triggers[KILN_TRIGGER_MAX];
static int g_trigger_count;

void kiln_trigger_init(void)
{
    memset(g_triggers, 0, sizeof(g_triggers));
    g_trigger_count = 0;
}

int kiln_trigger_add(const KilnTrigger *t)
{
    if (g_trigger_count >= KILN_TRIGGER_MAX) return 0;
    g_triggers[g_trigger_count++] = *t;
    return 1;
}

void kiln_trigger_update(fm_vec3_t player_pos, uint32_t player_handle,
                         float dt, fm_vec3_t *out_push_vel)
{
    (void)dt;
    if (out_push_vel) {
        out_push_vel->v[0] = 0; out_push_vel->v[1] = 0; out_push_vel->v[2] = 0;
    }
    for (int i = 0; i < g_trigger_count; i++) {
        KilnTrigger *t = &g_triggers[i];
        if (!t->active) continue;

        int inside = player_pos.v[0] >= t->mins.v[0] && player_pos.v[0] <= t->maxs.v[0] &&
                     player_pos.v[1] >= t->mins.v[1] && player_pos.v[1] <= t->maxs.v[1] &&
                     player_pos.v[2] >= t->mins.v[2] && player_pos.v[2] <= t->maxs.v[2];

        if (t->type == KILN_TRIG_PUSH) {
            if (inside && out_push_vel) {
                out_push_vel->v[0] += t->push_vel.v[0];
                out_push_vel->v[1] += t->push_vel.v[1];
                out_push_vel->v[2] += t->push_vel.v[2];
            }
            continue;
        }

        if (!inside) continue;
        if (t->type == KILN_TRIG_ONCE && t->fired) continue;

        t->fired = 1;
        if (player_handle != 0xFFFFFFFF) {
            kiln_event_post(player_handle, t->event_id, 0,
                           t->args, t->argc, 1);
        }
    }
}

void kiln_trigger_clear(void)
{
    g_trigger_count = 0;
    memset(g_triggers, 0, sizeof(g_triggers));
}