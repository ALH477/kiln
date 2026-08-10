// SPDX-License-Identifier: MPL-2.0
//
// m64_char.h — a character profile struct for games with selectable
// characters that have a passive ability + a charged special move.
//
// Why this lives in the engine, not the game: any party game (and most
// action games) with selectable characters has the same shape — a
// passive that tweaks a per-turn/per-frame rule, and a special that
// fires on demand with a cooldown. Putting the struct + cooldown state
// machine here means Ganja Goblin's four goblins, a hypothetical future
// fighting game's roster, or a tower-defence's hero units all share one
// well-tested shape rather than re-deriving the cooldown bookkeeping.
//
// What's deliberately not here:
//   - No character *data*. The engine defines the struct; the game
//     defines the table of profiles (Dank/Sparky/Moss/Glimmer for
//     Ganja Goblin) and the per-profile passive/special implementations.
//   - No per-character rendering. A character's visual is a model + a
//     portrait sprite; both are game-side assets.
//   - No AI. CPU players are game-side logic that calls the same
//     m64_char_charge / m64_char_try_special hooks a human player does.
//
// The passive is a function pointer the game supplies, called once per
// turn (or once per frame for tick-style passives). The special is a
// function pointer fired on demand via m64_char_try_special, gated by
// the cooldown counter. Charge accrues per bud collected, per the spec:
// "special move charged by collecting buds".

#ifndef M64_CHAR_H
#define M64_CHAR_H

#include <stdint.h>

typedef struct M64CharProfile M64CharProfile;
typedef struct M64CharState   M64CharState;

// Per-frame or per-turn tick for the passive. Called with the player's
// state pointer (game-side, opaque to the engine) and a delta-time for
// frame-tick passives; pass 0 for turn-tick passives.
typedef void (*M64CharPassiveFn)(M64CharState *self, void *game_player, float dt);

// Fire the special. Returns 1 if it actually fired (cooldown was ready,
// game-side conditions met), 0 otherwise. The game side owns the
// effect (Couch Lock freezes a rival, etc.); this function pointer is
// just the dispatch hook.
typedef int (*M64CharSpecialFn)(M64CharState *self, void *game_player, void *target);

struct M64CharProfile {
    uint8_t            id;        // game-defined; opaque to the engine
    const char        *name;
    const char        *passive_desc;
    const char        *special_desc;
    uint16_t           charge_threshold;  // buds needed to fire the special
    M64CharPassiveFn  passive;
    M64CharSpecialFn  special;
};

struct M64CharState {
    const M64CharProfile *profile;     // borrowed; game owns the table
    uint16_t              charge;       // current charge, in buds
    uint16_t              cooldown;     // turns until special can fire again
    uint8_t               active;       // 1 once a profile is assigned
};

void m64_char_init(M64CharState *s);
void m64_char_assign(M64CharState *s, const M64CharProfile *p);
void m64_char_tick_passive(M64CharState *s, void *game_player, float dt);
int  m64_char_try_special(M64CharState *s, void *game_player, void *target);
void m64_char_add_charge(M64CharState *s, uint16_t buds);
void m64_char_tick_cooldown(M64CharState *s);   // call once at end of turn

#endif // M64_CHAR_H