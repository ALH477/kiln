/* SPDX-License-Identifier: MIT
 *
 * kiln_inventory.h — item inventory. A fixed-size slot inventory for
 * collectible items (keys, keycards, armor, powerups). Modelled on Doom's
 * item flags + OoT's key inventory, reduced to what an N64 game can afford.
 *
 * ── Why a flat slot array, not a linked list ───────────────────────────
 * Doom uses a bitmask per item category; OoT uses a fixed array of item
 * slots. On N64 a flat array of {id, count} is cheaper than either: no
 * allocation, no tree walk, O(n) scan where n ≤ 32 is always < 1 µs on
 * the VR4300. Stack count supports ammo-as-item and consumable items.
 *
 * ── Game owns the item ids ──────────────────────────────────────────────
 * The engine defines no item ids. The game defines KEY_RED=1, ARMOR=10,
 * etc. and passes them to the add/has/consume functions. This keeps the
 * module generic across Doom (keycards, armor, megahealth), OoT (small
 * keys, boss keys, magic, arrows) and HL (HEV suit, medkits, ammo).
 */
#ifndef KILN_INVENTORY_H
#define KILN_INVENTORY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KILN_INV_SLOTS 32

typedef struct {
    uint16_t id;
    uint16_t count;
} KilnInvSlot;

typedef struct {
    KilnInvSlot slots[KILN_INV_SLOTS];
    uint8_t count;
} KilnInventory;

void kiln_inventory_init(KilnInventory *inv);

/** Add `count` of item `id`. If the item exists, stacks; if not, claims
 *  a new slot. Returns 1 if added, 0 if the inventory is full. */
int kiln_inventory_add(KilnInventory *inv, uint16_t id, uint16_t count);

/** Returns the count of item `id` (0 if not present). */
int kiln_inventory_has(const KilnInventory *inv, uint16_t id);

/** Consume `count` of item `id`. Returns 1 if consumed, 0 if not enough. */
int kiln_inventory_consume(KilnInventory *inv, uint16_t id, uint16_t count);

#ifdef __cplusplus
}
#endif

#endif /* KILN_INVENTORY_H */