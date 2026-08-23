/* SPDX-License-Identifier: MIT
 *
 * kiln_weapon.c — see kiln_weapon.h for the model.
 */

#include "kiln_weapon.h"

void kiln_weapon_init(KilnWeapon *w, int magazine_size,
                     float fire_cooldown, float reload_time)
{
    w->magazine_size  = magazine_size;
    w->magazine       = magazine_size;
    w->ammo           = magazine_size * 3;
    w->max_ammo       = magazine_size * 5;
    w->fire_cooldown  = fire_cooldown;
    w->reload_time    = reload_time;
    w->timer          = 0.0f;
    w->state          = KILN_WPN_IDLE;
}

int kiln_weapon_can_fire(const KilnWeapon *w)
{
    return w->state == KILN_WPN_IDLE && w->magazine > 0;
}

int kiln_weapon_fire(KilnWeapon *w)
{
    if (w->state != KILN_WPN_IDLE || w->magazine <= 0)
        return 0;
    w->magazine--;
    w->state = KILN_WPN_FIRING;
    w->timer = w->fire_cooldown;
    return 1;
}

int kiln_weapon_reload(KilnWeapon *w)
{
    if (w->state == KILN_WPN_RELOADING)
        return 0;
    if (w->magazine >= w->magazine_size)
        return 0;
    if (w->ammo <= 0)
        return 0;
    w->state = KILN_WPN_RELOADING;
    w->timer = w->reload_time;
    return 1;
}

void kiln_weapon_update(KilnWeapon *w, float dt)
{
    if (w->state == KILN_WPN_IDLE)
        return;

    w->timer -= dt;
    if (w->timer > 0.0f)
        return;

    if (w->state == KILN_WPN_FIRING) {
        w->state = KILN_WPN_IDLE;
        w->timer = 0.0f;
    } else if (w->state == KILN_WPN_RELOADING) {
        int needed = w->magazine_size - w->magazine;
        int take = needed < w->ammo ? needed : w->ammo;
        w->magazine += take;
        w->ammo     -= take;
        w->state = KILN_WPN_IDLE;
        w->timer = 0.0f;
    }
}