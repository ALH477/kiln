// SPDX-License-Identifier: MPL-2.0

#include "gg_spaces.h"

int gg_space_enter(KilnSpaceType type, GGPlayer *player)
{
    (void)player;  // Phase 5 will use this for status/item side effects.
    switch (type) {
        case KILN_SPACE_GROW:     return 3;
        case KILN_SPACE_DRY:      return -2;
        case KILN_SPACE_SPIRIT:   return 5;
        case KILN_SPACE_SHORTCUT: return 1;
        case KILN_SPACE_MINIGAME: return 4;   // stub: mini-game deferred to Phase 8
        case KILN_SPACE_TRADE:    return 0;   // stub: Phase 5's item-swap
        case KILN_SPACE_START:    return 1;   // tiny bonus for lapping the board
        default:                 return 0;
    }
}