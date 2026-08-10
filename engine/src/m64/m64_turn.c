// SPDX-License-Identifier: MPL-2.0

#include "m64_turn.h"

void m64_turn_init(M64Turn *t, uint8_t player_count, uint16_t max_rounds)
{
    if (player_count == 0) player_count = 1;
    t->player = 0;
    t->phase = M64_PHASE_ROLL;
    t->round = 1;
    t->max_rounds = max_rounds;
    t->player_count = player_count;
    t->next_phase_override = 0xFF;
}

void m64_turn_advance_phase(M64Turn *t)
{
    if (t->next_phase_override != 0xFF) {
        uint8_t np = t->next_phase_override;
        t->next_phase_override = 0xFF;
        if (np < M64_PHASE_COUNT) {
            t->phase = (M64TurnPhase)np;
            return;
        }
    }

    if (t->phase == M64_PHASE_END) {
        // Next player. Wrap if this was the last; advance the round.
        t->player++;
        if (t->player >= t->player_count) {
            t->player = 0;
            if (t->max_rounds == 0 || t->round < t->max_rounds) {
                t->round++;
            }
        }
        t->phase = M64_PHASE_ROLL;
        return;
    }

    t->phase = (M64TurnPhase)((int)t->phase + 1);
    if (t->phase >= M64_PHASE_COUNT) t->phase = M64_PHASE_END;
}

void m64_turn_skip_to_end(M64Turn *t)
{
    t->phase = M64_PHASE_END;
    t->next_phase_override = 0xFF;
}