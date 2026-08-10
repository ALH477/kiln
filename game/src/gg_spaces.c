// SPDX-License-Identifier: MPL-2.0

#include "gg_spaces.h"

int gg_space_enter(M64SpaceType type, GGPlayer *player)
{
    (void)player;  // Phase 5 will use this for status/item side effects.
    switch (type) {
        case M64_SPACE_GROW:     return 3;
        case M64_SPACE_DRY:      return -2;
        case M64_SPACE_SPIRIT:   return 5;
        case M64_SPACE_SHORTCUT: return 1;
        case M64_SPACE_MINIGAME: return 4;   // stub: mini-game deferred to Phase 8
        case M64_SPACE_TRADE:    return 0;   // stub: Phase 5's item-swap
        case M64_SPACE_START:    return 1;   // tiny bonus for lapping the board
        default:                 return 0;
    }
}