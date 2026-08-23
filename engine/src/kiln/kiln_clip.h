/* SPDX-License-Identifier: MIT
 *
 * kiln_clip.h — the collision layer. A clean-room analogue of id Tech 4's
 * idPhysics / CollisionModel trace API, reduced to what an OoT-style N64 game
 * actually needs and what a 93.75 MHz VR4300 can afford. See CLAUDE.md's
 * Phase C notes for what was and wasn't carried over.
 *
 * ── World = flat array of AABB brushes, no BSP ─────────────────────────
 * Doom 3's CollisionModel traces a TRM through an axial BSP built from the
 * map's brushes. That is the right design for a 2004 PC. On a 4 MB console
 * with rooms the size of OoT's, the BSP is overkill: a flat array of brush
 * AABBs per loaded room, slab-tested per trace, is cheaper to build, cheaper
 * to walk, and cache-friendlier (SOA mins/maxs reads one axis across all
 * brushes per slab pass). The trade is non-cube brushes lose internal
 * corners — accepted for rectangular OoT-style rooms, flagged in kiln_map.h.
 *
 * ── Slab-method swept AABB vs AABB, single precision ───────────────────
 * The classic Minkowski-sum reduction: expand the static brush by the moving
 * box's extents, then it's a ray-vs-AABB slab test. Six multiplies and a
 * handful of compares per brush per trace — no sqrt, no atan2, no libm. The
 * one division per axis per brush is the cost; with ~64 brushes that's 384
 * divides per trace, fine for a 60 fps game with one player + a handful of
 * projectiles. Single-precision epsilon is 1e-3 (not 1e-4): at s16.16 fixed
 * point world units, positions are ~10³ magnitude and a tighter epsilon
 * produces visible jitter at contact. The 1 mm slop is invisible to a player
 * and silences the "stuck against the wall" vibration that 1e-4 produces.
 *
 * ── SlideMove: iterative clip-and-retry ────────────────────────────────
 * Doom 3's `idPhysics_Player::SlideMove` sweeps, on a hit clips velocity
 * along the contact normal (removing the into-wall component), and retries
 * with the remaining velocity, up to a small iteration cap. We do the same:
 * up to `max_iter` (4 by convention) retries, then stop. This is what gives
 * a player the "slide along the wall instead of stopping dead" feel — a
 * single trace would freeze the player any time their velocity wasn't
 * perfectly perpendicular to a wall.
 *
 * ── What was NOT carried over from id Tech 4 ────────────────────────────
 * No rotation traces (Doom 3's `Rotation()`). No contents test (the `Contents()`
 * position test). No contact-point list (the `Contacts()` API). No TRM other
 * than an AABB — a real capsule with hemispherical caps is not in Doom 3
 * either (it uses an octagonal cylinder), and an octagonal cylinder costs
 * more than the rectangular rooms warrant. A game needing any of these
 * layers them on top by calling kiln_clip_box with a tighter mins/maxs, or
 * adds a sibling trace routine — the module is intentionally not a sealed
 * black box.
 */
#ifndef KILN_CLIP_H
#define KILN_CLIP_H

#include <stdint.h>
#include <t3d/t3dmath.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A world collision volume. `mins`/`maxs` are world-space; `surface` is an
 *  index into the kiln_surface table (see kiln_surface.h) — 0 means "default",
 *  passes the caller through to whatever surface the game registered at
 *  index 0 (typically stone/concrete). */
typedef struct {
    fm_vec3_t mins;
    fm_vec3_t maxs;
    uint8_t   surface;
    uint8_t   flags;
    uint8_t   _pad[2];
} KilnBrush;

/** Trace result. `fraction` is in [0, 1]: 1 = unobstructed full move, 0 = no
 *  movement possible (already in contact). `endpos` is `start + delta *
 *  fraction`. `normal` is the unit face normal of the blocking brush on a
 *  hit, or (0,0,0) on a clean trace — the caller must check `fraction < 1`
 *  before reading `normal`. `hitsurface` mirrors the blocking brush's
 *  `surface` field, for footstep SFX lookup. */
typedef struct {
    float     fraction;
    fm_vec3_t endpos;
    fm_vec3_t normal;
    uint8_t   hitsurface;
    uint8_t   _pad[3];
} KilnTrace;

/** Install the world's brushes. The pointer is borrowed and must outlive
 *  every subsequent trace. Pass NULL/0 to clear (all traces return fraction=1).
 *  The caller owns the array — typically the loaded rooms' brush arrays are
 *  concatenated into one module-static buffer by kiln_room's load/unload
 *  callbacks (see examples/clip-demo for the standalone case). */
void kiln_clip_set_world(const KilnBrush *brushes, uint16_t count);

/** Toggle the uniform-grid broadphase. When on, kiln_clip_set_world builds a 2D
 *  XZ grid over the brushes (Y ignored — rooms are short and a 3D grid blows
 *  the memory budget) and kiln_clip_box/slide/ground only slab-test brushes in
 *  cells overlapped by the swept AABB's XZ footprint. When off (the default),
 *  traces walk the flat array — the path the header comment above describes as
 *  fine for ~64 brushes. Opt in for larger worlds.
 *
 *  kiln_clip_ray (camera boom, line-of-sight) ALWAYS uses the flat walk, even
 *  with broadphase on — rays are 1/frame and grid-ray traversal (Amanatides-
 *  Woo) is more code than the win warrants. The header comment in kiln_camera
 *  explains why the boom is a ray, not a box. */
void kiln_clip_set_broadphase(int enabled);

/** Debug counter: how many brushes the most recent trace actually slab-tested.
 *  Zeroed at the start of every kiln_clip_box/ray/slide/ground call; incremented
 *  per brush examined. The physics-demo HUD reads this to show the broadphase
 *  win (flat walk = brush_count, grid path = brushes in overlapped cells).
 *  Not thread-safe; kiln_clip is module-global anyway. */
uint16_t kiln_clip_last_trace_brushes(void);

/** How many brushes are installed as the world right now.
 *
 *  Trivial, and it exists because its absence cost real time. An empty clip
 *  world is not an error here — every trace politely reports fraction 1, which
 *  is correct for a scene with no geometry — so a game whose collision failed
 *  to load behaves exactly like a game standing in an empty room: the player
 *  falls forever and nothing anywhere reports a problem. PetaByte Madness
 *  shipped its whole PM_SCREEN_PLAY that way, because a .map asset's filename
 *  did not match the path the ROM opened.
 *
 *  A HUD line reading `clip 0` distinguishes that from every other reason a
 *  first-person screen looks wrong, which no amount of moving the camera does. */
uint16_t kiln_clip_world_count(void);

/** Sweep an AABB (described by its `mins`/`maxs` relative to its center) from
 *  `start` to `end` (center positions). Returns the first brush contact along
 *  the sweep. mins/maxs are typically symmetric (e.g. {-8,-8,-8}..{8,8,8} for
 *  a 16-unit box) but need not be — a player's capsule-ish AABB has a smaller
 *  x/z than y. */
KilnTrace kiln_clip_box(fm_vec3_t start, fm_vec3_t end,
                      fm_vec3_t mins, fm_vec3_t maxs);

/** Zero-extent trace (a ray). Convenience: `kiln_clip_box` with mins=maxs=0,
 *  but a separate entry point because the camera boom and line-of-sight tests
 *  read clearer that way. */
KilnTrace kiln_clip_ray(fm_vec3_t start, fm_vec3_t end);

/** Iterative sweep-and-slide. Returns the position the box ends up at after
 *  up to `max_iter` traces, with velocity clipped along each contact normal.
 *  This is the Doom 3 SlideMove shape — what makes a player slide along a
 *  wall instead of stopping dead. `vel` is the desired full-frame displacement
 *  (NOT velocity — already multiplied by dt by the caller). */
fm_vec3_t kiln_clip_slide(fm_vec3_t pos, fm_vec3_t vel,
                         fm_vec3_t mins, fm_vec3_t maxs,
                         int max_iter);

/** Short downward probe (2 units) for "am I standing on something". The
 *  returned trace's `normal` is the ground normal — usually (0,1,0) on flat
 *  floors, canted on ramps (once kiln_map grows real ramps). `fraction < 1`
 *  means ground was found. */
KilnTrace kiln_clip_ground(fm_vec3_t pos, fm_vec3_t mins, fm_vec3_t maxs);

#ifdef __cplusplus
}
#endif

#endif /* KILN_CLIP_H */