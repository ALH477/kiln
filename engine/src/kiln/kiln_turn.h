// SPDX-License-Identifier: MIT
//
// kiln_turn.h — a turn/round state machine for party games.
//
// Complements fig_board: the board is *where you can go*, this is *whose
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
// frame; fig_turn just advances the state correctly when the game side
// tells it to.
//
// What's deliberately not here:
//   - No per-player state (buds, items, statuses). That lives in the game's
//     own player struct; fig_turn only tracks the active-player index and
//     the round counter.
//   - No mini-game scene switching. Phase 8 (future) handles that by
//     stacking a scene callback on top of fig_engine's frame begin/end.
//   - No AI. CPU goblins are game-side logic that drives the same
//     fig_turn_advance_phase calls a human player does.

#ifndef FIG_TURN_H
#define FIG_TURN_H


/* The prefix migration train (docs/NAMING.md section 9 step 2). Pulled in by
 * every public header (a quoted include, so it resolves both in this tree and
 * in the installed include/kiln prefix) rather than force-included by
 * kiln-inst.mk, because a
 * force-include only reaches builds that include that file — a Nix check or a
 * host build compiling a downstream's sources directly never saw it, and
 * PetaByte-Madness' pm-cine check is what proved that. Deleting the train is
 * still a scripted one-line removal from these headers plus the file itself.
 */
#include "kiln_compat.h"

#include <stdint.h>

typedef enum {
    FIG_PHASE_ROLL   = 0,
    FIG_PHASE_MOVE   = 1,
    FIG_PHASE_LAND   = 2,
    FIG_PHASE_EVENT  = 3,
    FIG_PHASE_END    = 4,
    FIG_PHASE_COUNT
} FigTurnPhase;

typedef struct {
    uint8_t       player;        // 0..player_count-1, whose turn
    FigTurnPhase  phase;
    uint16_t      round;         // 1-based; increments after player_count wraps
    uint16_t      max_rounds;    // 0 = unlimited
    uint8_t       player_count;
    // Phase-after-next override. Set by the game side to insert an extra
    // phase — e.g. a Harvest Event after LAND, or a forced mini-game. The
    // engine side never sets this; it just respects it. 0xFF = no override.
    uint8_t       next_phase_override;
} FigTurn;

void fig_turn_init(FigTurn *t, uint8_t player_count, uint16_t max_rounds);

// Advance one phase. ROLL -> MOVE -> LAND -> EVENT -> END -> (next player,
// ROLL). If next_phase_override is set when advance is called, that phase
// runs next instead of the linear successor; the override clears itself
// after firing.
void fig_turn_advance_phase(FigTurn *t);

// Skip the rest of this player's turn (Couch Lock). Jumps to END.
void fig_turn_skip_to_end(FigTurn *t);

// Convenience: is this the last phase before the turn ends?
// Useful for the game side to decide whether to draw the "End Turn" prompt.
static inline int fig_turn_is_last_phase(const FigTurn *t) {
    return t->phase == FIG_PHASE_EVENT;
}

// True if advancing past END will start a new round (i.e. current player is
// the last in the order). Useful for triggering end-of-round bookkeeping.
static inline int fig_turn_round_will_advance(const FigTurn *t) {
    return t->phase == FIG_PHASE_END && t->player + 1 >= t->player_count;
}

#endif // FIG_TURN_H