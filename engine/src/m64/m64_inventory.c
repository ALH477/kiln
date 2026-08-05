/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_inventory.c — see m64_inventory.h for the model.
 */

#include "m64_inventory.h"
#include <stddef.h>

void m64_inventory_init(M64Inventory *inv)
{
    for (int i = 0; i < M64_INV_SLOTS; i++) {
        inv->slots[i].id = 0;
        inv->slots[i].count = 0;
    }
    inv->count = 0;
}

int m64_inventory_add(M64Inventory *inv, uint16_t id, uint16_t count)
{
    if (id == 0) return 0;
    for (int i = 0; i < M64_INV_SLOTS; i++) {
        if (inv->slots[i].id == id) {
            inv->slots[i].count += count;
            return 1;
        }
    }
    for (int i = 0; i < M64_INV_SLOTS; i++) {
        if (inv->slots[i].id == 0) {
            inv->slots[i].id = id;
            inv->slots[i].count = count;
            inv->count++;
            return 1;
        }
    }
    return 0;
}

int m64_inventory_has(const M64Inventory *inv, uint16_t id)
{
    for (int i = 0; i < M64_INV_SLOTS; i++) {
        if (inv->slots[i].id == id)
            return inv->slots[i].count;
    }
    return 0;
}

int m64_inventory_consume(M64Inventory *inv, uint16_t id, uint16_t count)
{
    for (int i = 0; i < M64_INV_SLOTS; i++) {
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