// SPDX-License-Identifier: MPL-2.0

#include "gg_board1.h"

// A 12-node loop with one fork. Node 4 is the fork: branch 0 = long way
// (5 -> 6), branch 1 = shortcut (5 -> 7). Rejoins at node 7. Node 9 is a
// Mini-Game trigger, node 11 a Spirit space; the rest alternate Grow/Dry
// to exercise gg_spaces. World positions form a rough oval so the camera
// framing in Phase 4 has a sensible AABB.
const M64BoardNode gg_board1_nodes[GG_BOARD1_NODES] = {
    { M64_SPACE_START,    {{   0, 0,    0 }}, { 1 },         1 }, // 0
    { M64_SPACE_GROW,     {{  60, 0,    0 }}, { 2 },         1 }, // 1
    { M64_SPACE_DRY,      {{ 120, 0,    0 }}, { 3 },         1 }, // 2
    { M64_SPACE_GROW,     {{ 180, 0,    0 }}, { 4 },         1 }, // 3
    { M64_SPACE_SPIRIT,   {{ 240, 0,    0 }}, { 5, 6 },      2 }, // 4 fork
    { M64_SPACE_GROW,     {{ 280, 0,   60 }}, { 7 },         1 }, // 5 long
    { M64_SPACE_GROW,     {{ 280, 0,  -60 }}, { 7 },         1 }, // 6 short
    { M64_SPACE_DRY,      {{ 320, 0,    0 }}, { 8 },         1 }, // 7 rejoin
    { M64_SPACE_GROW,     {{ 280, 0,   80 }}, { 9 },         1 }, // 8
    { M64_SPACE_MINIGAME, {{ 200, 0,  120 }}, { 10 },        1 }, // 9 minigame
    { M64_SPACE_GROW,     {{ 100, 0,  120 }}, { 11 },        1 }, // 10
    { M64_SPACE_SPIRIT,   {{   0, 0,  120 }}, { 0 },         1 }, // 11 back to start
};

void gg_board1_init(M64Board *b)
{
    m64_board_init(b, gg_board1_nodes, GG_BOARD1_NODES, 0);
}