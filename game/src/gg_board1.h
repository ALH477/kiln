// SPDX-License-Identifier: MPL-2.0
//
// gg_board1.h — the first Ganja Goblin board. A hand-laid 12-node path with
// one fork (long way / shortcut) that rejoins, plus one Mini-Game trigger
// space and one Spirit space for variety. Phase 2 ships the topology; the
// rendering becomes 3D in Phase 3+ when the camera + goblin models land.

#ifndef GG_BOARD1_H
#define GG_BOARD1_H

#include <m64/m64_board.h>

#define GG_BOARD1_NODES 12

extern const M64BoardNode gg_board1_nodes[GG_BOARD1_NODES];

// Initialise a M64Board pointing at the static node table.
void gg_board1_init(M64Board *b);

#endif // GG_BOARD1_H