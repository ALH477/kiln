/* SPDX-License-Identifier: MIT
 *
 * kiln_weapon.c — see kiln_weapon.h for the model.
 */

#include "kiln_weapon.h"

void fig_weapon_init(FigWeapon *w, int magazine_size,
                     float fire_cooldown, float reload_time)
{
    w->magazine_size  = magazine_size;
    w->magazine       = magazine_size;
    w->ammo           = magazine_size * 3;
    w->max_ammo       = magazine_size * 5;
    w->fire_cooldown  = fire_cooldown;
    w->reload_time    = reload_time;
    w->timer          = 0.0f;
    w->state          = FIG_WPN_IDLE;
}

int fig_weapon_can_fire(const FigWeapon *w)
{
    return w->state == FIG_WPN_IDLE && w->magazine > 0;
}

int fig_weapon_fire(FigWeapon *w)
{
    if (w->state != FIG_WPN_IDLE || w->magazine <= 0)
        return 0;
    w->magazine--;
    w->state = FIG_WPN_FIRING;
    w->timer = w->fire_cooldown;
    return 1;
}

int fig_weapon_reload(FigWeapon *w)
{
    if (w->state == FIG_WPN_RELOADING)
        return 0;
    if (w->magazine >= w->magazine_size)
        return 0;
    if (w->ammo <= 0)
        return 0;
    w->state = FIG_WPN_RELOADING;
    w->timer = w->reload_time;
    return 1;
}

void fig_weapon_update(FigWeapon *w, float dt)
{
    if (w->state == FIG_WPN_IDLE)
        return;

    w->timer -= dt;
    if (w->timer > 0.0f)
        return;

    if (w->state == FIG_WPN_FIRING) {
        w->state = FIG_WPN_IDLE;
        w->timer = 0.0f;
    } else if (w->state == FIG_WPN_RELOADING) {
        int needed = w->magazine_size - w->magazine;
        int take = needed < w->ammo ? needed : w->ammo;
        w->magazine += take;
        w->ammo     -= take;
        w->state = FIG_WPN_IDLE;
        w->timer = 0.0f;
    }
}