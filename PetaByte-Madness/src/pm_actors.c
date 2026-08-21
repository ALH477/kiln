// SPDX-License-Identifier: MPL-2.0
//
// pm_actors.c — see pm_actors.h.

#include "pm_actors.h"

#include <libdragon.h>
#include <string.h>

#include "pm_types.h"
#include "pm_demons.h"
#include "pm_lab.h"

// Static because kiln_actor_system_init borrows this for the lifetime of
// the actor system and never copies it.
static KilnActorProfile g_table[PM_PROFILE_COUNT];

void pm_actors_init(KilnActor *pool, uint16_t pool_capacity)
{
    const int demons = PM_PROFILE_DEMON_COUNT;
    const int lab = pm_lab_profile_count();

    // If this ever fires, pm_types.h's enum and a module's profile array
    // have drifted apart. Loud at boot beats an imp spawning where a note
    // should be.
    assertf(demons + lab == PM_PROFILE_COUNT,
            "pm_actors: %d demon + %d lab profiles != PM_PROFILE_COUNT (%d)",
            demons, lab, PM_PROFILE_COUNT);

    memcpy(&g_table[0], pm_demons_profiles(),
           sizeof(KilnActorProfile) * (size_t)demons);
    memcpy(&g_table[PM_PROFILE_LAB_FIRST], pm_lab_profiles(),
           sizeof(KilnActorProfile) * (size_t)lab);

    kiln_actor_system_init(g_table, PM_PROFILE_COUNT, pool, pool_capacity);
}
