/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_weapons.c — see m64_weapons.h for the model.
 */

#include "m64_weapons.h"
#include <string.h>

void m64_weapons_init(M64WeaponSet *ws, const M64WeaponDef *defs, int count)
{
    memset(ws, 0, sizeof(*ws));
    if (count > M64_WEAPON_SLOTS) count = M64_WEAPON_SLOTS;
    for (int i = 0; i < count; i++) {
        ws->defs[i] = defs[i];
        m64_weapon_init(&ws->state[i], defs[i].magazine_size,
                        defs[i].fire_cooldown, defs[i].reload_time);
    }
    ws->slot_count = count;
    ws->active_slot = 0;
    ws->switch_timer = 0.0f;
}

int m64_weapons_switch(M64WeaponSet *ws, int slot)
{
    if (slot < 0 || slot >= ws->slot_count) return 0;
    if (slot == ws->active_slot) return 0;
    ws->active_slot = slot;
    ws->switch_timer = 0.3f;
    return 1;
}

int m64_weapons_next(M64WeaponSet *ws)
{
    int s = (ws->active_slot + 1) % ws->slot_count;
    return m64_weapons_switch(ws, s);
}

int m64_weapons_prev(M64WeaponSet *ws)
{
    int s = (ws->active_slot - 1 + ws->slot_count) % ws->slot_count;
    return m64_weapons_switch(ws, s);
}

int m64_weapons_fire(M64WeaponSet *ws)
{
    if (!m64_weapons_can_fire(ws)) return 0;
    return m64_weapon_fire(&ws->state[ws->active_slot]);
}

int m64_weapons_reload(M64WeaponSet *ws)
{
    return m64_weapon_reload(&ws->state[ws->active_slot]);
}

void m64_weapons_update(M64WeaponSet *ws, float dt)
{
    if (ws->switch_timer > 0.0f) ws->switch_timer -= dt;
    for (int i = 0; i < ws->slot_count; i++)
        m64_weapon_update(&ws->state[i], dt);
}

const M64WeaponDef *m64_weapons_active_def(const M64WeaponSet *ws)
{
    if (ws->active_slot < 0 || ws->active_slot >= ws->slot_count) return NULL;
    return &ws->defs[ws->active_slot];
}

M64Weapon *m64_weapons_active_state(M64WeaponSet *ws)
{
    if (ws->active_slot < 0 || ws->active_slot >= ws->slot_count) return NULL;
    return &ws->state[ws->active_slot];
}

int m64_weapons_can_fire(const M64WeaponSet *ws)
{
    if (ws->switch_timer > 0.0f) return 0;
    return m64_weapon_can_fire(&ws->state[ws->active_slot]);
}