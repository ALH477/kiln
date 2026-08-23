// SPDX-License-Identifier: MIT

#include "kiln_turn.h"

void kiln_turn_init(KilnTurn *t, uint8_t player_count, uint16_t max_rounds)
{
    if (player_count == 0) player_count = 1;
    t->player = 0;
    t->phase = KILN_PHASE_ROLL;
    t->round = 1;
    t->max_rounds = max_rounds;
    t->player_count = player_count;
    t->next_phase_override = 0xFF;
}

void kiln_turn_advance_phase(KilnTurn *t)
{
    if (t->next_phase_override != 0xFF) {
        uint8_t np = t->next_phase_override;
        t->next_phase_override = 0xFF;
        if (np < KILN_PHASE_COUNT) {
            t->phase = (KilnTurnPhase)np;
            return;
        }
    }

    if (t->phase == KILN_PHASE_END) {
        // Next player. Wrap if this was the last; advance the round.
        t->player++;
        if (t->player >= t->player_count) {
            t->player = 0;
            if (t->max_rounds == 0 || t->round < t->max_rounds) {
                t->round++;
            }
        }
        t->phase = KILN_PHASE_ROLL;
        return;
    }

    t->phase = (KilnTurnPhase)((int)t->phase + 1);
    if (t->phase >= KILN_PHASE_COUNT) t->phase = KILN_PHASE_END;
}

void kiln_turn_skip_to_end(KilnTurn *t)
{
    t->phase = KILN_PHASE_END;
    t->next_phase_override = 0xFF;
}