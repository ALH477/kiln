// SPDX-License-Identifier: MPL-2.0
//
// gg_boards.h — the table of selectable boards.
//
// The design doc asks for a Classic mode and "shorter Blitz Harvest
// boards". Those are one thing, not two: a board's node layout and its
// match length are chosen together (a 20-round match on the 8-node blitz
// loop is 2.5 laps of the same six spaces, which is not a shorter game so
// much as a more repetitive one). So there is one select screen listing
// boards, and each board carries its own round count.
//
// A board's actual node data lives in its own gg_boardN.c; this table is
// the index over them.

#ifndef GG_BOARDS_H
#define GG_BOARDS_H

#include <kiln/kiln_board.h>

typedef struct {
    const char         *name;
    const char         *blurb;      // one line shown under the list
    const KilnBoardNode *nodes;
    uint16_t            node_count;
    int16_t             start_node;
    uint16_t            rounds;     // match length for this board
} GGBoardDef;

#define GG_BOARD_COUNT 2
extern const GGBoardDef gg_board_defs[GG_BOARD_COUNT];

// Initialise `b` from the table entry at `index` (clamped into range).
void gg_boards_load(KilnBoard *b, int index);

#endif // GG_BOARDS_H
