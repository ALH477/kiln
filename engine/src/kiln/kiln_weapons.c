/* SPDX-License-Identifier: MIT
 *
 * kiln_weapons.c — see kiln_weapons.h for the model.
 */

#include "kiln_weapons.h"
#include <string.h>

void kiln_weapons_init(KilnWeaponSet *ws, const KilnWeaponDef *defs, int count)
{
    memset(ws, 0, sizeof(*ws));
    if (count > KILN_WEAPON_SLOTS) count = KILN_WEAPON_SLOTS;
    for (int i = 0; i < count; i++) {
        ws->defs[i] = defs[i];
        kiln_weapon_init(&ws->state[i], defs[i].magazine_size,
                        defs[i].fire_cooldown, defs[i].reload_time);
    }
    ws->slot_count = count;
    ws->active_slot = 0;
    ws->switch_timer = 0.0f;
}

int kiln_weapons_switch(KilnWeaponSet *ws, int slot)
{
    if (slot < 0 || slot >= ws->slot_count) return 0;
    if (slot == ws->active_slot) return 0;
    ws->active_slot = slot;
    ws->switch_timer = 0.3f;
    return 1;
}

int kiln_weapons_next(KilnWeaponSet *ws)
{
    int s = (ws->active_slot + 1) % ws->slot_count;
    return kiln_weapons_switch(ws, s);
}

int kiln_weapons_prev(KilnWeaponSet *ws)
{
    int s = (ws->active_slot - 1 + ws->slot_count) % ws->slot_count;
    return kiln_weapons_switch(ws, s);
}

int kiln_weapons_fire(KilnWeaponSet *ws)
{
    if (!kiln_weapons_can_fire(ws)) return 0;
    return kiln_weapon_fire(&ws->state[ws->active_slot]);
}

int kiln_weapons_reload(KilnWeaponSet *ws)
{
    return kiln_weapon_reload(&ws->state[ws->active_slot]);
}

void kiln_weapons_update(KilnWeaponSet *ws, float dt)
{
    if (ws->switch_timer > 0.0f) ws->switch_timer -= dt;
    for (int i = 0; i < ws->slot_count; i++)
        kiln_weapon_update(&ws->state[i], dt);
}

const KilnWeaponDef *kiln_weapons_active_def(const KilnWeaponSet *ws)
{
    if (ws->active_slot < 0 || ws->active_slot >= ws->slot_count) return NULL;
    return &ws->defs[ws->active_slot];
}

KilnWeapon *kiln_weapons_active_state(KilnWeaponSet *ws)
{
    if (ws->active_slot < 0 || ws->active_slot >= ws->slot_count) return NULL;
    return &ws->state[ws->active_slot];
}

int kiln_weapons_can_fire(const KilnWeaponSet *ws)
{
    if (ws->switch_timer > 0.0f) return 0;
    return kiln_weapon_can_fire(&ws->state[ws->active_slot]);
}