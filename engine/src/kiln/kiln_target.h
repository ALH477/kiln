/* SPDX-License-Identifier: MIT
 *
 * kiln_target.h — Z-targeting. A clean-room analogue of Ocarina of Time's
 * Z-target acquire/switch/reticle, reduced to what an N64 game can afford.
 *
 * ── Cone + range over category lists ──────────────────────────────────
 * OoT walks the actor pool every frame the Z-trigger is held and picks the
 * enemy inside a forward-facing cone and within a max range. We do the
 * same with kiln_actor_first/next over the ENEMY and NPC categories — no
 * spatial index, no kd-tree. At OoT enemy counts per room (≤ ~16) the
 * linear walk is cheaper than maintaining a structure.
 *
 * ── Acquire vs switch ─────────────────────────────────────────────────
 * `kiln_target_acquire` picks the best candidate from scratch (no current
 * lock). `kiln_target_switch` cycles to the next-best candidate in a stick
 * direction (OoT's right-tap-to-cycle), starting from the current lock so
 * the switch feels like "the next one over" rather than a re-pick.
 *
 * ── Reticle projection ───────────────────────────────────────────────
 * `kiln_target_draw_reticle` projects the locked actor's world position
 * through the scene's view+projection and draws a 2D reticle at the
 * resulting screen position. Falls back to clamping to screen edges when
 * the target is behind the camera — OoT clamps too, the alternative
 * (hiding the reticle off-screen) is the same UX but harder to read.
 */
#ifndef KILN_TARGET_H
#define KILN_TARGET_H

#include <libdragon.h>
#include <t3d/t3dmath.h>
#include "kiln_engine.h"
#include "kiln_actor.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Pick the best targetable actor in front of `eye` facing `fwd`, within
 *  `max_range` and inside a cone of half-angle `cone_half` (radians).
 *  Searches ENEMY then NPC category lists. Returns KILN_ACTOR_HANDLE_NONE
 *  if nothing qualifies. The "best" is the smallest angle to fwd among
 *  in-cone candidates; ties broken by range. */
KilnActorHandle kiln_target_acquire(fm_vec3_t eye, fm_vec3_t fwd,
                                  float cone_half, float max_range);

/** Cycle to the next-best target偏向 a stick direction. `cur` is the
 *  currently locked handle (may be NONE). `dir` is a unit-ish 2D vector in
 *  camera-right / camera-forward space — passing (1,0) picks the next
 *  candidate to the right of the current lock, (0,1) the next in front,
 *  etc. Returns the new handle, or NONE if nothing qualifies (also drops
 *  the lock). */
KilnActorHandle kiln_target_switch(KilnActorHandle cur, fm_vec3_t eye, fm_vec3_t fwd,
                                 fm_vec3_t cam_right, float cone_half, float max_range,
                                 fm_vec3_t dir);

/** Project a world position to screen coordinates and draw a reticle.
 *  `scene` provides the camera matrices; `world` is the target's world
 *  position; `screen_w`/`screen_h` are the viewport (e.g. 320×240). The
 *  reticle is four corner brackets drawn in the 2D pass with kiln_gui. If
 *  the target is behind the camera, the reticle clamps to the nearer
 *  screen edge. */
void kiln_target_draw_reticle(const KilnScene *scene, fm_vec3_t world,
                             int screen_w, int screen_h, color_t color);

#ifdef __cplusplus
}
#endif

#endif /* KILN_TARGET_H */