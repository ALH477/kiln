/* SPDX-License-Identifier: MIT
 *
 * kiln_player.h — the player locomotion state machine. A helper, not an
 * actor profile: the player IS an actor (category PLAYER), and its profile
 * update/draw call into kiln_player_* which owns the state machine. This
 * matches the plan's "player glue" division — the actor system stays
 * generic, the player-specific behaviour lives in its own module.
 *
 * ── State machine ─────────────────────────────────────────────────────
 * IDLE → WALK (stick > deadzone) → RUN (hold R or full tilt)
 * IDLE/WALK/RUN → ROLL (L-tap; brief invuln + small forward lunge)
 * IDLE/WALK/RUN → ATTACK (A-tap; brief hitbox in front)
 * any grounded → JUMP (B-tap; upward velocity)
 * JUMP → FALL (vy < 0) → grounded (kiln_clip_ground hit) → IDLE/WALK
 *
 * Each state has its own per-frame update; transitions are guarded by
 * input edges (kiln_input_pressed) and world state (kiln_clip_ground). The
 * full machine is in kiln_player_update, dispatched on KilnPlayerState.
 *
 * ── Footstep events ───────────────────────────────────────────────────
 * While walking/running on ground, kiln_player_update emits a FOOTSTEP
 * event to `self` (via kiln_event_post) at a cadence proportional to speed;
 * the actor's KilnActorEventFn (if registered) can play the surface's SFX.
 * The demo in examples/oot-demo wires this to kiln_surface_play_footstep.
 *
 * ── Why not a profile ─────────────────────────────────────────────────
 * A game may have multiple player-controlled actors (co-op, mount+dismount)
 * where the locomotion state machine is shared but the actor profile
 * differs. Keeping the state machine in a module that takes a KilnActor*
 * makes that a copy, not a fork. The actor's `state` block holds the
 * KilnPlayerState struct (see kiln_player_state_size for the asserted size).
 */
#ifndef KILN_PLAYER_H
#define KILN_PLAYER_H

#include <stdint.h>
#include <t3d/t3dmath.h>
#include "kiln_actor.h"
#include "kiln_input.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KILN_PLAYER_IDLE = 0,
    KILN_PLAYER_WALK,
    KILN_PLAYER_RUN,
    KILN_PLAYER_ROLL,
    KILN_PLAYER_ATTACK,
    KILN_PLAYER_JUMP,
    KILN_PLAYER_FALL,
} KilnPlayerState;

typedef struct {
    KilnPlayerState state;
    fm_vec3_t vel;
    float    yaw;        /* facing direction the locomotion drives        */
    float    state_t;    /* seconds in the current state                  */
    float    step_cd;    /* seconds until next FOOTSTEP event             */
    uint8_t  on_ground;  /* 1 if kiln_clip_ground hit last frame           */
    uint8_t  last_surf;  /* hitsurface underfoot last frame               */
} KilnPlayer;

/** Asserted-against-KILN_ACTOR_STATE_MAX size, so a profile can set
 *  state_size = KILN_PLAYER_STATE_SIZE at init. */
#define KILN_PLAYER_STATE_SIZE (sizeof(KilnPlayer))

/** Player half-extents for collision (a 16×32×16 box). Pass these as
 *  mins/maxs to kiln_clip_box/slide/ground. */
extern const fm_vec3_t KILN_PLAYER_MINS;
extern const fm_vec3_t KILN_PLAYER_MAXS;

/** Player event ids. The actor's KilnActorEventFn dispatches on these. */
enum {
    KILN_EV_PLAYER_FOOTSTEP = 0x10,
};

/** Get the player state from an actor's state block. */
static inline KilnPlayer *kiln_player_of(KilnActor *a)
{
    return (KilnPlayer *)a->state;
}

/** Set the camera-relative movement basis. Call once per frame before
 *  kiln_player_update so stick-up moves the player in the camera's forward
 *  direction. If never called, movement defaults to world axes (forward
 *  +Z, right +X). The y components are ignored (movement is on the XZ
 *  plane); pass camera forward/right projected to XZ. */
void kiln_player_set_camera_basis(fm_vec3_t fwd, fm_vec3_t right);

/** One-frame locomotion update. Reads kiln_input_get(port), integrates
 *  velocity against kiln_clip_slide, probes ground with kiln_clip_ground,
 *  advances the state machine, and posts FOOTSTEP events when due. The
 *  actor's transform is updated in place (pos + yaw). `port` is 1-based
 *  to match kiln_input_get; pass 0 for the default. */
void kiln_player_update(KilnActor *self, int port, float dt);

#ifdef __cplusplus
}
#endif

#endif /* KILN_PLAYER_H */