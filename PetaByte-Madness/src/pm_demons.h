// SPDX-License-Identifier: MPL-2.0
//
// pm_demons.h — the four demons, as m64_actor profiles.
//
// docs/VEIL_DESIGN.md §5 is the design; this is the runtime. Each species
// exists to teach one consequence of the veil being reciprocal — while it
// is up, they can see you too:
//
//   IMP        the teaching enemy. Knows exactly where you are the moment
//              you resolve it. Three of them, and one is close.
//   HELLHOUND  only *moves* while the veil is down. Veil up and it freezes
//              exactly where it is — visible, motionless, nearer than it
//              was. The stillness is the horror.
//   GARGOYLE   only ambushes what has looked at it. Veil down it is one of
//              the forty statues bolted around the level; veil up the
//              statue is gone and a gargoyle is standing there.
//   OVERLORD   pins the veil ON inside its halo. It cannot blind you, only
//              expose you — and take away the tool you manage every other
//              demon with.
//
// ── Where the port deviates ────────────────────────────────────────────
// The design gives every demon a body material flagged VEIL_PAL_PHANTOM
// and a separate eye material flagged VEIL_PAL_EYES, so the body costs
// zero while the veil is down and the eyes stay as pinpricks in the dark.
// The .glb drop has no per-material split and no CI4 conversion yet
// (VEIL_DESIGN.md §8 step 1 — "this is the real work; everything else is
// an afternoon"), so here:
//
//   * The body gate is real and works today: pm_veil_demon_submit()
//     decides whether t3d_model_draw is called at all, which is the whole
//     phantom rule and the whole frame-time argument.
//   * The eyes are drawn in the 2D pass as projected screen-space dots,
//     occlusion-tested with one m64_clip_ray per demon, rather than as
//     four triangles of an eye material. Cheaper than the real thing and
//     visually equivalent at 320x240 — but it is a stand-in, and it goes
//     away when the eye material lands.

#ifndef PM_DEMONS_H
#define PM_DEMONS_H

#include <t3d/t3dmath.h>
#include <m64/m64_actor.h>

#include "pm_veil.h"
#include "pm_types.h"

/** The profile table, indexed by PM_PROFILE_*. Hand this to
 *  m64_actor_system_init. Storage is module-static and outlives the actor
 *  system, as that function requires. */
const M64ActorProfile *pm_demons_profiles(void);

/** Load the four demon models out of DFS. Call after dfs_init and
 *  asset_init_compression(2), before spawning anything. Missing models are
 *  survivable: a demon with no mesh still updates and still gates on the
 *  veil, it just draws nothing. */
void pm_demons_load(void);

/** Point the demon code at the live veil and the player's eye position.
 *  Every species reads both every frame, and neither belongs in an actor's
 *  state block, so they are bound once per frame instead of copied 12
 *  times. */
void pm_demons_bind(const PMVeil *veil, fm_vec3_t player_eye);

/** Halo query, run once per frame BEFORE pm_veil_update: returns the
 *  strongest OVERLORD forcing strength at `pos`, 0 when none is in range.
 *  Feed it straight to pm_veil_force. */
float pm_demons_veil_force_at(fm_vec3_t pos);

/** 1 while any live demon currently has line of sight to the player. The
 *  reciprocity rule made observable — the HUD reads this, and so does
 *  anything that wants to punish standing in the open with the veil up. */
int pm_demons_player_seen(void);

/** Draw the eye pinpricks. Call inside the 2D pass (between
 *  m64_gui_begin and m64_gui_end), after the world has been drawn.
 *  See the deviation note above. */
void pm_demons_draw_eyes(const M64Scene *scene);

#endif // PM_DEMONS_H
