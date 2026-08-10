// SPDX-License-Identifier: MPL-2.0

#include "gg_board2.h"

// Blitz Harvest: an 8-node ring with one asymmetric shortcut.
//
//   0 START -> 1 GROW -> 2 TRADE -> 3 fork
//                                    branch 0 (long)  : 4 GROW -> 5 GROW -> 6 SPIRIT -> 7
//                                    branch 1 (short) : 7 directly, via the SHORTCUT node
//
// Node 7 is the SHORTCUT space itself and loops back to 0, so a player who
// takes branch 1 saves three spaces and skips both Grow spaces and the
// Spirit bonus — speed traded for yield, which is the choice the fork is
// there to pose. Positions are a compact ring so the board camera's fit
// radius is noticeably tighter than board 1's.
const M64BoardNode gg_board2_nodes[GG_BOARD2_NODES] = {
    { M64_SPACE_START,    {{   0, 0,    0 }}, { 1 },      1 }, // 0
    { M64_SPACE_GROW,     {{  80, 0,  -40 }}, { 2 },      1 }, // 1
    { M64_SPACE_TRADE,    {{ 140, 0,   20 }}, { 3 },      1 }, // 2
    { M64_SPACE_DRY,      {{ 160, 0,  100 }}, { 4, 7 },   2 }, // 3 fork
    { M64_SPACE_GROW,     {{ 110, 0,  160 }}, { 5 },      1 }, // 4 long
    { M64_SPACE_GROW,     {{  30, 0,  170 }}, { 6 },      1 }, // 5 long
    { M64_SPACE_SPIRIT,   {{ -40, 0,  120 }}, { 7 },      1 }, // 6 long
    { M64_SPACE_SHORTCUT, {{ -50, 0,   50 }}, { 0 },      1 }, // 7 rejoin / shortcut
};

void gg_board2_init(M64Board *b)
{
    m64_board_init(b, gg_board2_nodes, GG_BOARD2_NODES, 0);
}
