// SPDX-License-Identifier: MPL-2.0
//
// gg_spaces.h — per-space-type on-enter effects. The board module knows
// topology; this is the gameplay: what does landing on each space type do?
//
// Phase 2's table is the bare-minimum bud award. Phase 5 will extend this
// to trigger items, status effects, and mini-game transitions.

#ifndef GG_SPACES_H
#define GG_SPACES_H

#include "gg_types.h"
#include <m64/m64_board.h>

// Apply the on-enter effect of `type` to `player`. Returns the bud delta
// (negative for DRY spaces etc.). The caller is responsible for clamping
// the player's bud total to >= 0 after applying.
int gg_space_enter(M64SpaceType type, GGPlayer *player);

#endif // GG_SPACES_H