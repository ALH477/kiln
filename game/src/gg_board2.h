// SPDX-License-Identifier: MPL-2.0
//
// gg_board2.h — "Blitz Harvest", the design doc's short board. Eight
// nodes, a tight loop, and — unlike gg_board1's cosmetic fork, where both
// branches take the same number of spaces to rejoin — a real shortcut
// that skips three spaces. On a short match that asymmetry is the whole
// tension: the fast lane is genuinely fast, and it is lined with Dry
// spaces, so taking it costs buds.

#ifndef GG_BOARD2_H
#define GG_BOARD2_H

#include <m64/m64_board.h>

#define GG_BOARD2_NODES 8

extern const M64BoardNode gg_board2_nodes[GG_BOARD2_NODES];

void gg_board2_init(M64Board *b);

#endif // GG_BOARD2_H
