// SPDX-License-Identifier: MIT
//
// kiln_char.h — a character profile struct for games with selectable
// characters that have a passive ability + a charged special move.
//
// Why this lives in the engine, not the game: any party game (and most
// action games) with selectable characters has the same shape — a
// passive that tweaks a per-turn/per-frame rule, and a special that
// fires on demand with a cooldown. Putting the struct + cooldown state
// machine here means a party game's four-character roster, a hypothetical
// future fighting game's roster, or a tower-defence's hero units all share
// one well-tested shape rather than re-deriving the cooldown bookkeeping.
//
// What's deliberately not here:
//   - No character *data*. The engine defines the struct; the game
//     defines the table of profiles and the per-profile passive/special
//     implementations.
//   - No per-character rendering. A character's visual is a model + a
//     portrait sprite; both are game-side assets.
//   - No AI. CPU players are game-side logic that calls the same
//     fig_char_charge / fig_char_try_special hooks a human player does.
//
// The passive is a function pointer the game supplies, called once per
// turn (or once per frame for tick-style passives). The special is a
// function pointer fired on demand via fig_char_try_special, gated by
// the cooldown counter. Charge accrues per unit collected — a game defines
// what that unit represents (currency, resource pickups, whatever its own
// design calls it).

#ifndef FIG_CHAR_H
#define FIG_CHAR_H


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

typedef struct FigCharProfile FigCharProfile;
typedef struct FigCharState   FigCharState;

// Per-frame or per-turn tick for the passive. Called with the player's
// state pointer (game-side, opaque to the engine) and a delta-time for
// frame-tick passives; pass 0 for turn-tick passives.
typedef void (*FigCharPassiveFn)(FigCharState *self, void *game_player, float dt);

// Fire the special. Returns 1 if it actually fired (cooldown was ready,
// game-side conditions met), 0 otherwise. The game side owns the
// effect (a status effect on a rival, a burst of movement, etc.); this
// function pointer is just the dispatch hook.
typedef int (*FigCharSpecialFn)(FigCharState *self, void *game_player, void *target);

struct FigCharProfile {
    uint8_t            id;        // game-defined; opaque to the engine
    const char        *name;
    const char        *passive_desc;
    const char        *special_desc;
    uint16_t           charge_threshold;  // charge units needed to fire the special
    FigCharPassiveFn  passive;
    FigCharSpecialFn  special;
};

struct FigCharState {
    const FigCharProfile *profile;     // borrowed; game owns the table
    uint16_t              charge;       // current charge, in game-defined units
    uint16_t              cooldown;     // turns until special can fire again
    uint8_t               active;       // 1 once a profile is assigned
};

void fig_char_init(FigCharState *s);
void fig_char_assign(FigCharState *s, const FigCharProfile *p);
void fig_char_tick_passive(FigCharState *s, void *game_player, float dt);
int  fig_char_try_special(FigCharState *s, void *game_player, void *target);
void fig_char_add_charge(FigCharState *s, uint16_t amount);
void fig_char_tick_cooldown(FigCharState *s);   // call once at end of turn

#endif // FIG_CHAR_H