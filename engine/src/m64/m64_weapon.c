/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_weapon.c — see m64_weapon.h for the model.
 */

#include "m64_weapon.h"

void m64_weapon_init(M64Weapon *w, int magazine_size,
                     float fire_cooldown, float reload_time)
{
    w->magazine_size  = magazine_size;
    w->magazine       = magazine_size;
    w->ammo           = magazine_size * 3;
    w->max_ammo       = magazine_size * 5;
    w->fire_cooldown  = fire_cooldown;
    w->reload_time    = reload_time;
    w->timer          = 0.0f;
    w->state          = M64_WPN_IDLE;
}

int m64_weapon_can_fire(const M64Weapon *w)
{
    return w->state == M64_WPN_IDLE && w->magazine > 0;
}

int m64_weapon_fire(M64Weapon *w)
{
    if (w->state != M64_WPN_IDLE || w->magazine <= 0)
        return 0;
    w->magazine--;
    w->state = M64_WPN_FIRING;
    w->timer = w->fire_cooldown;
    return 1;
}

int m64_weapon_reload(M64Weapon *w)
{
    if (w->state == M64_WPN_RELOADING)
        return 0;
    if (w->magazine >= w->magazine_size)
        return 0;
    if (w->ammo <= 0)
        return 0;
    w->state = M64_WPN_RELOADING;
    w->timer = w->reload_time;
    return 1;
}

void m64_weapon_update(M64Weapon *w, float dt)
{
    if (w->state == M64_WPN_IDLE)
        return;

    w->timer -= dt;
    if (w->timer > 0.0f)
        return;

    if (w->state == M64_WPN_FIRING) {
        w->state = M64_WPN_IDLE;
        w->timer = 0.0f;
    } else if (w->state == M64_WPN_RELOADING) {
        int needed = w->magazine_size - w->magazine;
        int take = needed < w->ammo ? needed : w->ammo;
        w->magazine += take;
        w->ammo     -= take;
        w->state = M64_WPN_IDLE;
        w->timer = 0.0f;
    }
}