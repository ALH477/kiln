// SPDX-License-Identifier: MPL-2.0
//
// pm_lab.h — the playable lab, before the transformation.
//
// The one section of the intro the player drives. He walks Horner around
// the station in first person, reads what is lying about, and activates
// the MRI when he decides to — it is not a timer and not a trigger volume.
// A man doing this to himself has to press the button.
//
// ── First person, and why that is the right call here ──────────────────
// The opening cinematic shows Horner in third person and then flies the
// camera into his head (pm_demo's LAB_CINE shot ends there; pm_lab_enter
// picks up from that exact eye). From then until the beach you never see
// his body — which is what makes the reveal land, because the first time
// you see what he became, it is killing someone with it.
//
// It is also the cheap answer: m64_fpscam, m64_clip and the whole
// first-person path are already built and working in this ROM.
//
// ── Readables are actors, not a special case ───────────────────────────
// Every readable is an M64_ACTOR_CAT_PROP actor, which m64_context_scan
// already reports as M64_CTX_USE. So "what does A do right now" is the
// engine's existing answer, and pm_lab only decides what USE means for
// the profile under the cursor: a note opens m64_dialogue, the MRI ends
// the section.
//
// ── The collision is hand-authored ─────────────────────────────────────
// dank_lab.obj is geometry, not brushes — there is no collision mesh in
// the drop and m64_map only reads Quake .map files. The lab is a box with
// a few obstacles, so six brushes plus a handful of blockers is both
// cheaper and more predictable than deriving collision from 1,538
// triangles. If the lab ever gets complicated enough that this stops
// being true, author it as a .map and use m64_map instead.

#ifndef PM_LAB_H
#define PM_LAB_H

#include <m64/m64_actor.h>
#include <m64/m64_fpscam.h>
#include <m64/m64_input.h>
#include <m64/m64_engine.h>

// ── The room's real extents ──────────────────────────────────────────────
// dank_lab.obj is authored in centimetres and lands in the world at
// baseScale 64 per metre (see docs/ASSET_PIPELINE.md), so its bbox
// X -7.06..2.80 m, Y 0..2.50, Z -2.30..2.68 becomes the numbers below.
//
// Exposed here (rather than kept private to pm_lab.c, where they used to
// live) so pm_demo.c's lab_cine camera and pm_intake.c's intake camera
// derive from the SAME source the room's own collision does, instead of
// each independently typing a number that can drift from what is actually
// drawn — which is exactly what happened when the shipped model was
// pm_world.py's procedural box while these described dank_lab.obj: every
// consumer of "the lab's extents" except pm_lab.c itself was keyed to a
// room shape nothing drew.
#define PM_LAB_REAL_X0   (-452.0f)
#define PM_LAB_REAL_X1    (179.0f)
#define PM_LAB_REAL_Y0      (0.0f)
#define PM_LAB_REAL_Y1    (160.0f)
#define PM_LAB_REAL_Z0   (-147.0f)
#define PM_LAB_REAL_Z1    (171.0f)

// ── Set dressing near the MRI, shared across every screen that shows it ─
// dank_lab_gen.py authors in centimetres at the same 0.64-per-cm scale the
// bbox above was derived from (world = cm * 0.01 m/cm * 64 world/m). Reading
// the generator's own numbers here — workstations()'s STOOL_X/STOOL_Z for the
// desk, a hand-picked spot beside scanner()'s R_OUT=92 clearance for the
// arms — keeps pm_intake.c's walk-up path, pm_lab.c's playable-lab arms, and
// pm_demo.c's LAB_CINE arms agreeing by construction rather than by each
// guessing its own copy.
#define PM_LAB_DESK_X    (113.92f)  // dank_lab_gen.py workstations(): STOOL_X (178) * 0.64
#define PM_LAB_DESK_Y     (61.44f)  // standing/seated hip height: 96 cm * 0.64
#define PM_LAB_DESK_Z     (21.76f)  // STOOL_Z (34) * 0.64

#define PM_LAB_ARMS_X     (12.8f)   // mounted just clear of the scanner's cx=-70
#define PM_LAB_ARMS_Y     (89.6f)   // high on the wall, reaching down to table height
#define PM_LAB_ARMS_Z      (0.0f)

/** The lab's actor profiles, appended after the demons'. Handed to
 *  pm_actors_table, which is what m64_actor_system_init actually sees. */
const M64ActorProfile *pm_lab_profiles(void);
int pm_lab_profile_count(void);

/** Give `cam` the player's body: the AABB and the movement constants, both
 *  sized for this world's 64 units per metre. Does NOT position the camera
 *  or touch the clip world — callers follow it with m64_fpscam_snap.
 *
 *  Every first-person screen must go through this. PM_SCREEN_LAB gets it
 *  via pm_lab_enter; PM_SCREEN_PLAY calls it directly, because the two used
 *  to hold separate copies of these numbers at two different world scales
 *  and the gameplay one was wrong. See the definition. */
void pm_lab_body(M64FpsCam *cam);

/** Eye height above the feet, in world units — what to add to a floor-level
 *  spawn point to get the position m64_fpscam_snap wants. */
float pm_lab_eye_height(void);

/** Install the lab's clip world, spawn its props, and place the player at
 *  `eye` looking along `yaw` — the pose the opening cinematic's camera
 *  finished on, so the cut into first person does not jump. */
void pm_lab_enter(M64FpsCam *cam, fm_vec3_t eye, float yaw);

/** Despawn the lab's props and drop its clip world. */
void pm_lab_leave(void);

/** One frame of the playable lab. Returns 1 on the frame the player
 *  activates the MRI, which is what ends the section. */
int pm_lab_update(M64FpsCam *cam, const M64Input *in, float dt);

/** Draw the lab. Call inside the 3D pass. */
void pm_lab_draw3d(void);

/** Draw the prompt and any open dialogue. Call inside the GUI pass. */
void pm_lab_draw2d(int screen_w, int screen_h);

/** Where the opening cinematic should leave the camera — the eye position
 *  pm_lab_enter expects. Exposed so pm_demo's LAB_CINE shot ends exactly
 *  here rather than somewhere approximately like it. */
fm_vec3_t pm_lab_start_eye(void);
float     pm_lab_start_yaw(void);

#endif // PM_LAB_H
