// SPDX-License-Identifier: MPL-2.0

#include "gg_boards.h"
#include "gg_board1.h"
#include "gg_board2.h"

const GGBoardDef gg_board_defs[GG_BOARD_COUNT] = {
    {
        .name       = "GANJA GROVE",
        .blurb      = "12 spaces, split path",
        .nodes      = gg_board1_nodes,
        .node_count = GG_BOARD1_NODES,
        .start_node = 0,
        .rounds     = 10,
    },
    {
        .name       = "BLITZ HARVEST",
        .blurb      = "8 spaces, real shortcut",
        .nodes      = gg_board2_nodes,
        .node_count = GG_BOARD2_NODES,
        .start_node = 0,
        .rounds     = 5,
    },
};

void gg_boards_load(KilnBoard *b, int index)
{
    if (index < 0) index = 0;
    if (index >= GG_BOARD_COUNT) index = GG_BOARD_COUNT - 1;
    const GGBoardDef *d = &gg_board_defs[index];
    kiln_board_init(b, d->nodes, d->node_count, d->start_node);
}
