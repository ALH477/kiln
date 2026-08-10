// SPDX-License-Identifier: MPL-2.0
//
// gg_turn.h — the Ganja Goblin turn driver. Wraps m64_turn's generic state
// machine with the game-side work each phase needs:
//
//   ROLL  -> roll the die, store the result
//   MOVE  -> advance the active player's token `roll` spaces along the board
//            (branch 0 for now; Phase 4 adds player-choice at forks)
//   LAND  -> apply the space's on-enter effect via gg_space_enter
//   EVENT -> tick scheduled board events (Phase 5: Harvest / Party Mode)
//   END   -> m64_turn handles the player/round transition
//
// One call to gg_turn_step() advances one phase. The caller (main loop)
// decides whether to step every frame (auto demo) or gate on input.

#ifndef GG_TURN_H
#define GG_TURN_H

#include "gg_types.h"
#include <m64/m64_board.h>
#include <m64/m64_turn.h>
#include <m64/m64_dice.h>
#include <m64/m64_rng.h>
#include <m64/m64_char.h>

typedef struct {
    M64Turn     turn;
    M64Dice     die;
    M64Rng      rng;
    int         last_roll;     // result of the most recent ROLL phase
    int         last_player;   // who rolled it
    float       roll_anim_t;   // seconds the die has been spinning (UI only)

    // MOVE is stepped one space per gg_turn_step call rather than resolved
    // in one go. Two reasons: the token visibly walks the board instead of
    // teleporting, and — the real one — a fork can only be offered to the
    // player at the moment the token reaches it, which a single-shot loop
    // has no place to pause in.
    int         steps_left;      // spaces still to walk this turn
    int8_t      branch_choice;   // -1 = undecided; set by the UI at a fork
    uint8_t     awaiting_branch; // 1 when MOVE is blocked on a fork choice
    // One M64CharState per player. The active player's char is ticked in
    // ROLL (passive) and given a chance to fire its special after LAND.
    M64CharState chars[GG_PLAYERS];
    // Rolling HUD log — last 6 events, for visible state-machine audit.
    char        log[6][40];
    int         log_head;
} GGTurnState;

void gg_turn_init(GGTurnState *s, uint16_t max_rounds);

// Reset a match in progress back to round 1 without re-seeding the RNG —
// a second match in one session should not replay the first one's rolls.
void gg_turn_restart(GGTurnState *s, uint16_t max_rounds);
// One phase transition. Reads + writes `players` (active player's token
// and buds), reads `board` for topology and space types.
void gg_turn_step(GGTurnState *s, const M64Board *board, GGPlayer *players);

void gg_turn_log(GGTurnState *s, const char *fmt, ...);

#endif // GG_TURN_H