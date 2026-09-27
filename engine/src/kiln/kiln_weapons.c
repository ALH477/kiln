/* SPDX-License-Identifier: MIT
 *
 * kiln_weapons.c — see kiln_weapons.h for the model.
 */

#include "kiln_weapons.h"
#include <string.h>

void fig_weapons_init(FigWeaponSet *ws, const FigWeaponDef *defs, int count)
{
    memset(ws, 0, sizeof(*ws));
    if (count > FIG_WEAPON_SLOTS) count = FIG_WEAPON_SLOTS;
    for (int i = 0; i < count; i++) {
        ws->defs[i] = defs[i];
        fig_weapon_init(&ws->state[i], defs[i].magazine_size,
                        defs[i].fire_cooldown, defs[i].reload_time);
    }
    ws->slot_count = count;
    ws->active_slot = 0;
    ws->switch_timer = 0.0f;
}

int fig_weapons_switch(FigWeaponSet *ws, int slot)
{
    if (slot < 0 || slot >= ws->slot_count) return 0;
    if (slot == ws->active_slot) return 0;
    ws->active_slot = slot;
    ws->switch_timer = 0.3f;
    return 1;
}

int fig_weapons_next(FigWeaponSet *ws)
{
    int s = (ws->active_slot + 1) % ws->slot_count;
    return fig_weapons_switch(ws, s);
}

int fig_weapons_prev(FigWeaponSet *ws)
{
    int s = (ws->active_slot - 1 + ws->slot_count) % ws->slot_count;
    return fig_weapons_switch(ws, s);
}

int fig_weapons_fire(FigWeaponSet *ws)
{
    if (!fig_weapons_can_fire(ws)) return 0;
    return fig_weapon_fire(&ws->state[ws->active_slot]);
}

int fig_weapons_reload(FigWeaponSet *ws)
{
    return fig_weapon_reload(&ws->state[ws->active_slot]);
}

void fig_weapons_update(FigWeaponSet *ws, float dt)
{
    if (ws->switch_timer > 0.0f) ws->switch_timer -= dt;
    for (int i = 0; i < ws->slot_count; i++)
        fig_weapon_update(&ws->state[i], dt);
}

const FigWeaponDef *fig_weapons_active_def(const FigWeaponSet *ws)
{
    if (ws->active_slot < 0 || ws->active_slot >= ws->slot_count) return NULL;
    return &ws->defs[ws->active_slot];
}

FigWeapon *fig_weapons_active_state(FigWeaponSet *ws)
{
    if (ws->active_slot < 0 || ws->active_slot >= ws->slot_count) return NULL;
    return &ws->state[ws->active_slot];
}

int fig_weapons_can_fire(const FigWeaponSet *ws)
{
    if (ws->switch_timer > 0.0f) return 0;
    return fig_weapon_can_fire(&ws->state[ws->active_slot]);
}