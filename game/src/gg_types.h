// SPDX-License-Identifier: MPL-2.0
//
// gg_types.h — shared game-side types for Ganja Goblin.
//
// The engine's m64_turn tracks only whose turn it is and what phase;
// per-player state (position on the board, buds, inventory, status
// effects) lives here, on the game side. This matches the engine's
// design rule: m64_turn is generic, the player struct is game-specific.

#ifndef GG_TYPES_H
#define GG_TYPES_H

#include <stdint.h>
#include <m64/m64_inventory.h>
#include <m64/m64_board.h>

#define GG_PLAYERS       4
#define GG_STARTING_BUDS  0
#define GG_DEFAULT_ROUNDS 10

// Item ids — game-defined, opaque to the engine. Phase 5 fills in the rest;
// for now the inventory is just buds (which are tracked separately as the
// score currency, not as inventory items).
typedef enum {
    GG_ITEM_NONE        = 0,
    GG_ITEM_SMOKE_BOMB  = 1,
    GG_ITEM_STICKY_RESIN = 2,
    GG_ITEM_MUNCHIES_BAG = 3,
    GG_ITEM_SPORE_CLOUD = 4,
    GG_ITEM_CRYSTAL_SEED = 5,
} GGItem;

// Status effects. Phase 5 implements the tick/expire logic; for now the
// enum is here so the player struct has a stable shape.
typedef enum {
    GG_STATUS_NONE       = 0,
    GG_STATUS_COUCH_LOCK = 1,
    GG_STATUS_MUNCHIES   = 2,
    GG_STATUS_PARANOIA   = 3,
} GGStatus;

typedef struct {
    int16_t      node;          // index into the board
    int32_t      buds;          // currency / score
    M64Inventory inv;           // items
    GGStatus     status;
    uint8_t      status_turns;  // turns remaining for the active status
    // Per-turn passive bonuses accrued by the active character's passive
    // hook during ROLL, consumed during MOVE/LAND. Cleared at END.
    int8_t       pending_move_bonus;
    int8_t       pending_bud_bonus;
    // Sparky's "every 3rd turn" counter — increments per turn taken.
    uint8_t      turns_played;
} GGPlayer;

#endif // GG_TYPES_H