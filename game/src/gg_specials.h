// SPDX-License-Identifier: MPL-2.0
//
// gg_specials.h — the four goblins' passive + special implementations.
//
// Each passive fires once per turn (called from gg_turn's ROLL phase via
// kiln_char_tick_passive) and tweaks the active player's per-turn state.
// Each special fires on demand via kiln_char_try_special; the game side
// (gg_turn) decides when to attempt it.
//
// The "game_player" pointer passed to these hooks is the GGPlayer* that
// owns the KilnCharState; the "target" pointer is a rival GGPlayer* for
// specials that act on someone else (Couch Lock, Resin Trap).

#ifndef GG_SPECIALS_H
#define GG_SPECIALS_H

#include <kiln/kiln_char.h>
#include "gg_types.h"

// Set the player array the specials can reach into. Call once at boot
// before any special fires.
void gg_specials_set_players(GGPlayer *players, int count);

// Passives — called per turn for the active player.
void gg_passive_dank   (KilnCharState *self, void *game_player, float dt);
void gg_passive_sparky (KilnCharState *self, void *game_player, float dt);
void gg_passive_moss   (KilnCharState *self, void *game_player, float dt);
void gg_passive_glimmer(KilnCharState *self, void *game_player, float dt);

// Specials — fire on demand, return 1 if actually fired.
int  gg_special_dank   (KilnCharState *self, void *game_player, void *target);
int  gg_special_sparky (KilnCharState *self, void *game_player, void *target);
int  gg_special_moss   (KilnCharState *self, void *game_player, void *target);
int  gg_special_glimmer(KilnCharState *self, void *game_player, void *target);

#endif // GG_SPECIALS_H