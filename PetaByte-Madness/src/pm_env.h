// SPDX-License-Identifier: MPL-2.0
//
// pm_env.h — the moonlit island exterior: sky, sea, and the light that
// makes them one place.
//
// ── What this fixes ────────────────────────────────────────────────────
// Before this module the flyover drew the island and six palms into a
// near-black clear colour under the ENGINE DEFAULT rig — grey ambient 45
// and one white light from (1,1,1), which is a midday sun. The lit faces
// read green, every face turned away collapsed to about rgb(16,16,16), and
// below the island's rim there was nothing at all. It looked like broken
// geometry floating in a void, and it was reported as exactly that. The
// geometry was never wrong; the environment was missing.
//
// ── Three things, one horizon colour ───────────────────────────────────
// The dome's lowest ring, the sea's outermost ring and the fog are all
// PM_ENV_HORIZON. That three-way match is what makes the sea end without
// an edge: the water fades into fog, the fog is the colour of the sky
// where it meets the water, and the outer rim of a few-thousand-unit
// polygon disc becomes invisible. tools/blender/pm_env.py owns the value
// and this header quotes it; changing one without the others puts a
// visible ring around the world.
//
// ── The sky is a backdrop, not geometry ────────────────────────────────
// The dome is drawn first, unlit, with depth testing OFF and centred on
// the camera. So it cannot be reached, cannot be clipped into, and never
// occludes anything — every later draw simply paints over it. That is
// also why its radius does not matter and is not tuned against far_z.
//
// ── The sea's vertex colours are ceilings ──────────────────────────────
// The water is drawn `texel * vertex colour` with the scrolling foam
// texture, so a vertex colour is what a CREST looks like there, and flat
// water between crests is that colour at the texture's floor (~20%). Read
// pm_env.py's palette as moonlit spray rather than as sea, and see
// tools/gen_textures.py's FOAM_FLOOR for the other half of the contract.
//
// ── Used by every island scene ─────────────────────────────────────────
// The boot flyover, the sub arrival and the beach are the same island on
// the same night, so they share one rig rather than three sets of tuned
// numbers that drift apart.

#ifndef PM_ENV_H
#define PM_ENV_H

#include <kiln/kiln_engine.h>

/** The horizon colour, shared by the sky's lowest ring, the sea's outer
 *  ring and the fog. Mirrors HORIZON in tools/blender/pm_env.py. */
#define PM_ENV_HORIZON_R 38
#define PM_ENV_HORIZON_G 48
#define PM_ENV_HORIZON_B 74

/** Load the sky and sea. Call during a fade or a black frame, like every
 *  other preload in this game — see pm_models.h on where the hitch is
 *  free. Safe to call more than once. */
void pm_env_init(void);

/** Install the night rig on `scene`: moon key light, fill, ambient, fog
 *  and clear colour. Call from a shot's `setup`, before the first frame
 *  that draws it.
 *
 *  Sets light_count to 2. The fill exists because one light alone leaves
 *  every shadowed face at flat ambient, and on a curved island that reads
 *  as a hole in the mesh rather than as darkness — which is precisely how
 *  the original bug report described it. */
void pm_env_night(KilnScene *scene);

/** Put `scene` back to an ordinary interior rig: one light, grey ambient,
 *  no fog. The reel cuts between the island and the lab, and KilnScene is
 *  reused across shots — so without this the lab inherits moonlight and
 *  sea fog and reads as a cave. Called for every shot that is not marked
 *  `exterior`; see PMDemoShot in pm_demo.h. */
void pm_env_interior(KilnScene *scene);

/** The lab's interior rig, with the KEY AND FILL DIRECTIONS taken from the
 *  fixtures dank_lab_gen.py actually authored — the two with the most influence
 *  at `focus`, which should be roughly what the shot is framing.
 *
 *  Not "install the generator's rig": the lab's vertex colours are already
 *  baked with that rig, so re-applying its brightness and tint double-darkens
 *  and double-tints the room. See pm_env.c's comment above the function; it is
 *  the whole reason this room read as unlit. What the rig is good for is
 *  direction, so the runtime's highlights land where the baked ones already
 *  are.
 *
 *  Degrades to exactly pm_env_interior when no fixture reaches `focus`. */
void pm_env_interior_from_rig(KilnScene *scene, fm_vec3_t focus);

/** Advance the swell and the foam scroll. Call once per frame, before the
 *  3D pass. */
void pm_env_update(float dt);

/** Draw the sky. Call FIRST inside the 3D pass, before any world
 *  geometry: it disables depth so that whatever follows paints over it. */
void pm_env_draw_sky(const KilnScene *scene);

/** Draw the lightning channel, if one is striking this frame. Call with
 *  the world geometry — it is depth-tested so the temple can occlude it. */
void pm_env_draw_bolt(const KilnScene *scene);

/** Draw the sea. Call AFTER the island, so the island's own depth rejects
 *  the water behind it instead of the water overdrawing the shore. */
void pm_env_draw_sea(void);

/** Lightning intensity this frame, 0..1. Non-zero only during the few
 *  frames of a strike. Exposed so gameplay can react to being lit up —
 *  the veil's whole premise is that being visible has a cost. */
float pm_env_bolt(void);

/** Whether the swell is actually displacing vertices — 0 if the sea model
 *  is missing or its vertex buffer could not be snapshotted. The debug
 *  overlay reads this: a still sea and an absent sea look identical in a
 *  screenshot, and this game has already lost time to exactly that class
 *  of ambiguity (see pm_models_status). */
int pm_env_swell_active(void);

#endif // PM_ENV_H
