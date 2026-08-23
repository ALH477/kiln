/* SPDX-License-Identifier: MIT
 *
 * kiln_physics.h — a small Half-Life 2-style rigid-body physics layer for
 * dynamic props, on top of kiln_clip. See CLAUDE.md's Phase E notes for the
 * model. This is the "physics engine" half of the user's request; the
 * "optimized" half is kiln_clip's optional broadphase grid, and the
 * "capable of being toggled" is kiln_physics_set_enabled.
 *
 * ── Why AABB-only rigid bodies (no rotation) ──────────────────────────
 * HL2's VPhysics (Havok) gives every prop a full oriented-box collision
 * shape that tumbles realistically. That requires OBB-vs-AABB and OBB-vs-
 * OBB swept traces, plus angular integration. On a 93.75 MHz VR4300 with
 * no FPU divide and 4 MB of RDRAM, that is not on the menu — and kiln_clip
 * has no rotation traces by design (see kiln_clip.h's "NOT carried over"
 * list). What kiln_clip DOES have is a fast swept-AABB trace + SlideMove.
 *
 * The workaround: bodies are axis-aligned boxes that never rotate. They
 * still fall under gravity, slide along walls (kiln_clip_slide), stack on
 * each other (box-vs-box impulse resolution along the contact axis), get
 * punted by the player (the gravity-gun feel), and rest stably on the
 * ground (sleeping). A crate hitting the corner of a wall doesn't tumble
 * — it stops or slides. That is the visible difference from HL2, and on a
 * console this size it is the right trade. The header exists to make that
 * trade explicit, not to apologise for it.
 *
 * ── What was NOT carried over from VPhysics ────────────────────────────
 * No rotation, no angular velocity, no torque. No constraints (hinges,
 * ball-sockets, ragdolls). No continuous collision detection between
 * bodies — pair resolution is overlap-test + positional correction, fine
 * at N64 velocities (a few hundred units/sec) with a 60 Hz step. No
 * buoyancy, no fluid forces. No vehicles. A game needing any of these
 * layers them on top by extending KilnPhysicsBody or adding a sibling
 * solver — the module is intentionally not a sealed black box.
 *
 * ── Why a runtime toggle, not just "don't call step" ──────────────────
 * kiln_physics_set_enabled(w, 0) makes kiln_physics_step a no-op: bodies
 * keep their state, gravity stops, collisions stop, sleeping stays. The
 * use case is a cutscene that freezes props mid-air, or a pause menu —
 * the world is still rendered, it just doesn't advance. Re-enable and
 * everything resumes from where it stopped. Cheaper than re-integrating
 * from a saved state and matches what HL2's `phys_timescale 0` does.
 *
 * ── For non-player actors ─────────────────────────────────────────────
 * kiln_player keeps its own direct kiln_clip calls — the player's state
 * machine (IDLE/WALK/RUN/ROLL/ATTACK/JUMP/FALL) and footstep event
 * plumbing stay tight, and oot-demo doesn't regress. kiln_physics is for
 * the crates, barrels, props, and projectiles the world is full of. Two
 * locomotion paths, each documented; a future game that wants the player
 * to BE a physics body can call kiln_physics from inside its player_update
 * instead of kiln_player, but that's a per-game choice, not the default.
 */
#ifndef KILN_PHYSICS_H
#define KILN_PHYSICS_H

#include <stdint.h>
#include <t3d/t3dmath.h>
#include "kiln_clip.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KILN_PHYS_STATIC   = 0, /* not integrated; contributes to body-vs-body    */
    KILN_PHYS_DYNAMIC  = 1, /* integrated: gravity, world slide, body-vs-body */
    KILN_PHYS_KINEMATIC = 2, /* moved externally; pushes dynamics, not pushed  */
} KilnPhysType;

/** A rigid body. AABB-only — `mins`/`maxs` are half-extents relative to
 *  `pos` (the box's center), the same convention as kiln_clip_box. The
 *  caller fills one of these per body and hands the array to
 *  kiln_physics_init; kiln_physics_step mutates `pos`/`vel`/`on_ground`/
 *  `last_surf`/`sleeping`/`sleep_timer` in place. */
typedef struct {
    fm_vec3_t pos;          /* center, world-space                              */
    fm_vec3_t vel;          /* units/sec                                        */
    fm_vec3_t mins, maxs;   /* half-extents relative to pos                     */
    float     mass;         /* 0 for static/kinematic                           */
    float     inv_mass;     /* 1/mass for dynamic, 0 for static/kinematic       */
    float     restitution;  /* 0 = no bounce, 1 = perfectly elastic             */
    float     friction;     /* 0 = no friction, 1 = full stop on contact        */
    uint8_t   type;         /* KilnPhysType                                      */
    uint8_t   on_ground;    /* set by step from kiln_clip_ground                 */
    uint8_t   last_surf;    /* hitsurface underfoot last step                   */
    uint8_t   sleeping;     /* 1 once vel² has been < eps for SLEEP_FRAMES      */
    float     sleep_timer;  /* seconds at rest                                  */
} KilnPhysicsBody;

/** The world. Caller-owned storage; the engine holds a pointer. `enabled`
 *  is the runtime toggle. `broadphase` is a mirror of kiln_clip's flag, kept
 *  here only so the demo HUD can show it without a separate getter. */
typedef struct {
    KilnPhysicsBody *bodies;
    uint16_t count;
    uint16_t capacity;
    float    gravity;       /* default -500 units/s^2 (HL2-ish)                 */
    float    fixed_dt;      /* integration step; 1/60 by default                */
    int      enabled;       /* 0 = step is a no-op, 1 = integrate               */
    int      broadphase;    /* informational; kiln_clip owns the real flag       */
} KilnPhysicsWorld;

/** Bind the body pool. `bodies` is borrowed and must outlive every
 *  subsequent step. Asserts cap > 0. Sets sensible defaults (gravity
 *  -500, fixed_dt 1/60, enabled 1, broadphase 0) — override after init. */
void kiln_physics_init(KilnPhysicsWorld *w, KilnPhysicsBody *bodies, uint16_t cap);

/** The toggle. When 0, kiln_physics_step returns immediately without
 *  touching body state — bodies freeze in place. When 1, normal stepping. */
void kiln_physics_set_enabled(KilnPhysicsWorld *w, int enabled);

/** Claim a body slot and fill in the per-body fields. Returns NULL if the
 *  pool is full. `type` drives integration; mass is ignored for static/
 *  kinematic (inv_mass is set to 0). `half` is the box's half-extents
 *  (symmetric — a 16-unit crate passes {8,8,8}). */
KilnPhysicsBody *kiln_physics_spawn(KilnPhysicsWorld *w, KilnPhysType type,
                                  fm_vec3_t pos, fm_vec3_t half_extents,
                                  float mass);

/** Convenience: applies an impulse to a body's velocity (just `vel += imp *
 *  inv_mass`). The gravity-gun punt in examples/physics-demo uses this. */
void kiln_physics_apply_impulse(KilnPhysicsBody *b, fm_vec3_t imp);

/** Advance the world by `dt`. Slices `dt` into `fixed_dt` substeps for
 *  stable integration; clamps to a small max substep count so a long
 *  frame doesn't spiral. Order per substep:
 *    1. gravity integration on awake dynamic bodies
 *    2. world collision via kiln_clip_slide (per body)
 *    3. ground probe via kiln_clip_ground (per body)
 *    4. body-vs-body pairwise overlap + impulse resolution
 *    5. friction + sleep update
 *  Sleeping bodies are skipped until something disturbs them (a contact
 *  from another body, or kiln_physics_apply_impulse). When `enabled` is 0
 *  the function returns without touching state. */
void kiln_physics_step(KilnPhysicsWorld *w, float dt);

#ifdef __cplusplus
}
#endif

#endif /* KILN_PHYSICS_H */