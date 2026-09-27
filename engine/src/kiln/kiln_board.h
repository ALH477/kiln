// SPDX-License-Identifier: MIT
//
// kiln_board.h — a branching path graph for party-game boards.
//
// Not modelled on OoT or any specific game — there isn't an obvious
// precedent in this engine's existing primitives. fig_room is AABB-overlap
// streaming of free-walk 3D spaces; fig_map parses Quake brushes. Neither
// has any concept of a discrete "space" you land on, which is the central
// abstraction of a Mario-Party-style board. This module is that
// abstraction: a flat array of nodes (each carrying a space type + a world
// position + a small fixed list of outgoing edges) plus helpers for moving
// a token along the graph one step at a time.
//
// What's deliberately not here:
//   - No spatial index. The board is small (<= 128 nodes by default) and
//     movement is graph-walking, not point-in-space, so an index buys
//     nothing.
//   - No camera or rendering. The board is pure topology + world
//     positions; Phase 4's FIG_CAM_BOARD mode projects the camera from the
//     board's AABB.
//   - No item/space-event logic. The on-enter hook is a function pointer
//     the game side supplies, keyed by space type — see kiln_turn.h for
//     where that hooks into the turn loop.
//   - No runtime authored boards. Boards are static const tables at the
//     scale a party game ships. A board editor is a build-tool concern,
//     not an engine one.

#ifndef FIG_BOARD_H
#define FIG_BOARD_H

#include <stdint.h>
#include <libdragon.h>      // fm_vec3_t lives in libdragon's fast-math
#include <t3d/t3d.h>        // Tiny3D typedefs fm_vec3_t -> T3DVec3

#define FIG_BOARD_MAX_NODES   128
#define FIG_BOARD_MAX_EDGES     4   // branch factor; 1=linear, 2-3=branch, 4=hub

typedef enum {
    FIG_SPACE_START     = 0,
    FIG_SPACE_GOOD      = 1,
    FIG_SPACE_BAD       = 2,
    FIG_SPACE_TRADE     = 3,
    FIG_SPACE_BONUS     = 4,
    FIG_SPACE_MINIGAME  = 5,
    FIG_SPACE_SHORTCUT  = 6,
    FIG_SPACE_COUNT
} FigSpaceType;

typedef struct {
    FigSpaceType type;
    fm_vec3_t    pos;            // world-space placement for rendering + camera
    int16_t      next[FIG_BOARD_MAX_EDGES]; // -1 = no edge
    uint8_t      next_count;     // 1 = linear, >1 = branch
} FigBoardNode;

typedef struct {
    const FigBoardNode *nodes;
    uint16_t            node_count;
    int16_t             start_node;
    // Cached AABB computed once at init from the node positions. Used by
    // Phase 4's board camera to frame the whole board.
    fm_vec3_t           aabb_min;
    fm_vec3_t           aabb_max;
} FigBoard;

// Initialise a board from a node array. Computes the AABB. node_count must
// be > 0 and <= FIG_BOARD_MAX_NODES; start_node must be a valid index.
void fig_board_init(FigBoard *b, const FigBoardNode *nodes, uint16_t node_count,
                    int16_t start_node);

// Walk one edge from `cur` along branch index `branch`. If branch is out of
// range for the node's next_count, returns cur (no movement). The game-side
// turn loop is responsible for picking the branch — typically 0 for linear
// movement, or a player-choice predicate at a fork.
int16_t fig_board_step(const FigBoard *b, int16_t cur, uint8_t branch);

// World-space position of a node. Returns the start node's position if the
// index is out of range, so a buggy caller renders a stale token rather
// than reading off the end of the node array.
const fm_vec3_t *fig_board_pos(const FigBoard *b, int16_t idx);

#endif // FIG_BOARD_H