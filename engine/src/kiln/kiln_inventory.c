/* SPDX-License-Identifier: MIT
 *
 * kiln_inventory.c — see kiln_inventory.h for the model.
 */

#include "kiln_inventory.h"
#include <stddef.h>

void kiln_inventory_init(KilnInventory *inv)
{
    for (int i = 0; i < KILN_INV_SLOTS; i++) {
        inv->slots[i].id = 0;
        inv->slots[i].count = 0;
    }
    inv->count = 0;
}

int kiln_inventory_add(KilnInventory *inv, uint16_t id, uint16_t count)
{
    if (id == 0) return 0;
    for (int i = 0; i < KILN_INV_SLOTS; i++) {
        if (inv->slots[i].id == id) {
            inv->slots[i].count += count;
            return 1;
        }
    }
    for (int i = 0; i < KILN_INV_SLOTS; i++) {
        if (inv->slots[i].id == 0) {
            inv->slots[i].id = id;
            inv->slots[i].count = count;
            inv->count++;
            return 1;
        }
    }
    return 0;
}

int kiln_inventory_has(const KilnInventory *inv, uint16_t id)
{
    for (int i = 0; i < KILN_INV_SLOTS; i++) {
        if (inv->slots[i].id == id)
            return inv->slots[i].count;
    }
    return 0;
}

int kiln_inventory_consume(KilnInventory *inv, uint16_t id, uint16_t count)
{
    for (int i = 0; i < KILN_INV_SLOTS; i++) {
        if (inv->slots[i].id == id && inv->slots[i].count >= count) {
            inv->slots[i].count -= count;
            if (inv->slots[i].count == 0) {
                inv->slots[i].id = 0;
                inv->count--;
            }
            return 1;
        }
    }
    return 0;
}