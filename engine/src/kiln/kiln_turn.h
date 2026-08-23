// SPDX-License-Identifier: MIT
//
// kiln_turn.h — a turn/round state machine for party games.
//
// Complements kiln_board: the board is *where you can go*, this is *whose
// turn it is and what phase of their turn*. Mario Party's loop is:
//
//   ROLL  -> player rolls the die
//   MOVE  -> token advances N spaces (with branch choices at forks)
//   LAND  -> the space's on-enter effect fires (gain/lose buds, item, etc.)
//   EVENT -> scheduled board events / harvest / party-mode trigger
//   END   -> advance to the next player (or next round if all players done)
//
// This module owns that state machine and nothing else. The game side
// supplies the per-phase work (rolling the die, picking a branch, applying
// the space's effect) by reading `phase` and the current player index each
// frame; kiln_turn just advances the state correctly when the game side
// tells it to.
//
// What's deliberately not here:
//   - No per-player state (buds, items, statuses). That lives in the game's
//     own player struct; kiln_turn only tracks the active-player index and
//     the round counter.
//   - No mini-game scene switching. Phase 8 (future) handles that by
//     stacking a scene callback on top of kiln_engine's frame begin/end.
//   - No AI. CPU goblins are game-side logic that drives the same
//     kiln_turn_advance_phase calls a human player does.

#ifndef KILN_TURN_H
#define KILN_TURN_H

#include <stdint.h>

typedef enum {
    KILN_PHASE_ROLL   = 0,
    KILN_PHASE_MOVE   = 1,
    KILN_PHASE_LAND   = 2,
    KILN_PHASE_EVENT  = 3,
    KILN_PHASE_END    = 4,
    KILN_PHASE_COUNT
} KilnTurnPhase;

typedef struct {
    uint8_t       player;        // 0..player_count-1, whose turn
    KilnTurnPhase  phase;
    uint16_t      round;         // 1-based; increments after player_count wraps
    uint16_t      max_rounds;    // 0 = unlimited
    uint8_t       player_count;
    // Phase-after-next override. Set by the game side to insert an extra
    // phase — e.g. a Harvest Event after LAND, or a forced mini-game. The
    // engine side never sets this; it just respects it. 0xFF = no override.
    uint8_t       next_phase_override;
} KilnTurn;

void kiln_turn_init(KilnTurn *t, uint8_t player_count, uint16_t max_rounds);

// Advance one phase. ROLL -> MOVE -> LAND -> EVENT -> END -> (next player,
// ROLL). If next_phase_override is set when advance is called, that phase
// runs next instead of the linear successor; the override clears itself
// after firing.
void kiln_turn_advance_phase(KilnTurn *t);

// Skip the rest of this player's turn (Couch Lock). Jumps to END.
void kiln_turn_skip_to_end(KilnTurn *t);

// Convenience: is this the last phase before the turn ends?
// Useful for the game side to decide whether to draw the "End Turn" prompt.
static inline int kiln_turn_is_last_phase(const KilnTurn *t) {
    return t->phase == KILN_PHASE_EVENT;
}

// True if advancing past END will start a new round (i.e. current player is
// the last in the order). Useful for triggering end-of-round bookkeeping.
static inline int kiln_turn_round_will_advance(const KilnTurn *t) {
    return t->phase == KILN_PHASE_END && t->player + 1 >= t->player_count;
}

#endif // KILN_TURN_H