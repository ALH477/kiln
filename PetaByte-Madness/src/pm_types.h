// SPDX-License-Identifier: MPL-2.0
//
// pm_types.h — shared game-side types for PetaByte Madness.
//
// Same division of labour the Ganja Goblin project uses: engine modules
// stay generic, and anything that only means something inside THIS game
// lives here. kiln_actor knows about categories and pools; it does not know
// what a hellhound is.

#ifndef PM_TYPES_H
#define PM_TYPES_H

#include <stdint.h>

#define PM_SCREEN_W 320
#define PM_SCREEN_H 240

// Actor profile ids — indices into the one table kiln_actor_system_init
// gets (built by pm_actors.c from each module's own block). The demons
// come first because they were here first and because pm_demons.c indexes
// its model table by profile id directly.
//
// The layout is contiguous and the boundary markers are load-bearing:
// pm_lab.c's profile array is indexed by (id - PM_PROFILE_LAB_FIRST), so
// inserting a demon without moving the marker would silently shift every
// lab prop by one.
enum {
    PM_PROFILE_IMP = 0,
    PM_PROFILE_HELLHOUND,
    PM_PROFILE_GARGOYLE,
    PM_PROFILE_OVERLORD,
    PM_PROFILE_DEMON_COUNT,

    PM_PROFILE_LAB_FIRST = PM_PROFILE_DEMON_COUNT,
    PM_PROFILE_NOTE = PM_PROFILE_LAB_FIRST,
    PM_PROFILE_MRI,

    PM_PROFILE_COUNT,
};

// Event ids for kiln_event. Kept above 0x20 by the same convention the FPS
// example uses, so engine-internal ids never collide with game ones.
enum {
    PM_EV_FOOTSTEP    = 0x20,
    PM_EV_DEMON_NOTICE = 0x21,  // "it has seen you" — fires once per reveal
    PM_EV_DEMON_ATTACK = 0x22,
};

// Surface ids for kiln_surface. The lab is steel plate and standing water;
// the flooded sections are the ones you can hear yourself in.
enum {
    PM_SURF_DEFAULT = 0,
    PM_SURF_DECK    = 1,  // steel plate
    PM_SURF_WATER   = 2,  // standing bilge water
    PM_SURF_STONE   = 3,  // the statuary the gargoyles hide among
};

// The player. Horner is first-person, so his transform lives in the
// KilnFpsCam and only the gameplay state is here.
typedef struct {
    int32_t health;
    int32_t max_health;
    // Seconds of air. The veil is free on the hardware and must not be
    // free in the fiction (VEIL_DESIGN.md §6); this is the second cost,
    // on top of being seen — holding it up burns air faster.
    float   air;
    float   max_air;
    uint8_t seen;  // 1 while any demon currently has line of sight
} PMPlayer;

#endif // PM_TYPES_H
