// SPDX-License-Identifier: MPL-2.0

#include "m64_char.h"

void m64_char_init(M64CharState *s)
{
    s->profile = 0;
    s->charge = 0;
    s->cooldown = 0;
    s->active = 0;
}

void m64_char_assign(M64CharState *s, const M64CharProfile *p)
{
    s->profile = p;
    s->charge = 0;
    s->cooldown = 0;
    s->active = p ? 1 : 0;
}

void m64_char_tick_passive(M64CharState *s, void *game_player, float dt)
{
    if (!s->active || !s->profile || !s->profile->passive) return;
    s->profile->passive(s, game_player, dt);
}

int m64_char_try_special(M64CharState *s, void *game_player, void *target)
{
    if (!s->active || !s->profile || !s->profile->special) return 0;
    if (s->cooldown > 0) return 0;
    if (s->charge < s->profile->charge_threshold) return 0;
    int fired = s->profile->special(s, game_player, target);
    if (fired) {
        s->charge = 0;
        // Cooldown is per-spec; Ganja Goblin's specials are once-per-match
        // candidates, so 3 turns is a reasonable default for "fired but
        // not permanently spent". Game-side specials can override by
        // writing s->cooldown themselves before returning.
        if (s->cooldown == 0) s->cooldown = 3;
    }
    return fired;
}

void m64_char_add_charge(M64CharState *s, uint16_t buds)
{
    if (!s->active) return;
    // Saturate at the threshold so the bar doesn't wrap.
    uint32_t sum = (uint32_t)s->charge + buds;
    if (s->profile && sum > s->profile->charge_threshold)
        sum = s->profile->charge_threshold;
    s->charge = (uint16_t)sum;
}

void m64_char_tick_cooldown(M64CharState *s)
{
    if (s->cooldown > 0) s->cooldown--;
}