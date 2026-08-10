// SPDX-License-Identifier: MPL-2.0
//
// pm_actors.h — the one actor profile table.
//
// m64_actor_system_init takes a single contiguous array indexed by
// profile_id, and it must outlive the actor system. But the profiles
// themselves belong to the modules that implement them: the demons' update
// rules are pm_demons' business, the lab's props are pm_lab's.
//
// So each module keeps its own block and this assembles them, once, in the
// order pm_types.h's enum declares. Copying seven small structs at boot is
// cheaper than the alternative — one module owning every other module's
// callbacks, which is how a "types.c" grows into the file nobody wants to
// touch.

#ifndef PM_ACTORS_H
#define PM_ACTORS_H

#include <m64/m64_actor.h>

/** Build the combined table and hand it, with the caller's pool, to
 *  m64_actor_system_init. Asserts the assembled table is exactly
 *  PM_PROFILE_COUNT entries — the enum and the modules' blocks are two
 *  places that have to agree, and disagreeing silently means every actor
 *  past the seam spawns as the wrong type. */
void pm_actors_init(M64Actor *pool, uint16_t pool_capacity);

#endif // PM_ACTORS_H
