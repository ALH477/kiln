// SPDX-License-Identifier: MIT

#include "kiln_char.h"

void kiln_char_init(KilnCharState *s)
{
    s->profile = 0;
    s->charge = 0;
    s->cooldown = 0;
    s->active = 0;
}

void kiln_char_assign(KilnCharState *s, const KilnCharProfile *p)
{
    s->profile = p;
    s->charge = 0;
    s->cooldown = 0;
    s->active = p ? 1 : 0;
}

void kiln_char_tick_passive(KilnCharState *s, void *game_player, float dt)
{
    if (!s->active || !s->profile || !s->profile->passive) return;
    s->profile->passive(s, game_player, dt);
}

int kiln_char_try_special(KilnCharState *s, void *game_player, void *target)
{
    if (!s->active || !s->profile || !s->profile->special) return 0;
    if (s->cooldown > 0) return 0;
    if (s->charge < s->profile->charge_threshold) return 0;
    int fired = s->profile->special(s, game_player, target);
    if (fired) {
        s->charge = 0;
        // Cooldown is per-spec; a special that isn't meant to be permanently
        // spent still wants some default gap before it can fire again, so 3
        // turns is a reasonable one. Game-side specials can override by
        // writing s->cooldown themselves before returning.
        if (s->cooldown == 0) s->cooldown = 3;
    }
    return fired;
}

void kiln_char_add_charge(KilnCharState *s, uint16_t amount)
{
    if (!s->active) return;
    // Saturate at the threshold so the bar doesn't wrap.
    uint32_t sum = (uint32_t)s->charge + amount;
    if (s->profile && sum > s->profile->charge_threshold)
        sum = s->profile->charge_threshold;
    s->charge = (uint16_t)sum;
}

void kiln_char_tick_cooldown(KilnCharState *s)
{
    if (s->cooldown > 0) s->cooldown--;
}