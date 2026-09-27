/* SPDX-License-Identifier: MIT
 *
 * kiln_player.h — the player locomotion state machine. A helper, not an
 * actor profile: the player IS an actor (category PLAYER), and its profile
 * update/draw call into fig_player_* which owns the state machine. This
 * matches the plan's "player glue" division — the actor system stays
 * generic, the player-specific behaviour lives in its own module.
 *
 * ── State machine ─────────────────────────────────────────────────────
 * IDLE → WALK (stick > deadzone) → RUN (hold R or full tilt)
 * IDLE/WALK/RUN → ROLL (L-tap; brief invuln + small forward lunge)
 * IDLE/WALK/RUN → ATTACK (A-tap; brief hitbox in front)
 * any grounded → JUMP (B-tap; upward velocity)
 * JUMP → FALL (vy < 0) → grounded (fig_clip_ground hit) → IDLE/WALK
 *
 * Each state has its own per-frame update; transitions are guarded by
 * input edges (fig_input_pressed) and world state (fig_clip_ground). The
 * full machine is in fig_player_update, dispatched on FigPlayerState.
 *
 * ── Footstep events ───────────────────────────────────────────────────
 * While walking/running on ground, fig_player_update emits a FOOTSTEP
 * event to `self` (via fig_event_post) at a cadence proportional to speed;
 * the actor's FigActorEventFn (if registered) can play the surface's SFX.
 * The demo in examples/oot-demo wires this to fig_surface_play_footstep.
 *
 * ── Why not a profile ─────────────────────────────────────────────────
 * A game may have multiple player-controlled actors (co-op, mount+dismount)
 * where the locomotion state machine is shared but the actor profile
 * differs. Keeping the state machine in a module that takes a FigActor*
 * makes that a copy, not a fork. The actor's `state` block holds the
 * FigPlayerState struct (see fig_player_state_size for the asserted size).
 */
#ifndef FIG_PLAYER_H
#define FIG_PLAYER_H

#include <stdint.h>
#include <t3d/t3dmath.h>
#include "kiln_actor.h"
#include "kiln_input.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FIG_PLAYER_IDLE = 0,
    FIG_PLAYER_WALK,
    FIG_PLAYER_RUN,
    FIG_PLAYER_ROLL,
    FIG_PLAYER_ATTACK,
    FIG_PLAYER_JUMP,
    FIG_PLAYER_FALL,
} FigPlayerState;

typedef struct {
    FigPlayerState state;
    fm_vec3_t vel;
    float    yaw;        /* facing direction the locomotion drives        */
    float    state_t;    /* seconds in the current state                  */
    float    step_cd;    /* seconds until next FOOTSTEP event             */
    uint8_t  on_ground;  /* 1 if fig_clip_ground hit last frame           */
    uint8_t  last_surf;  /* hitsurface underfoot last frame               */
} FigPlayer;

/** Asserted-against-FIG_ACTOR_STATE_MAX size, so a profile can set
 *  state_size = FIG_PLAYER_STATE_SIZE at init. */
#define FIG_PLAYER_STATE_SIZE (sizeof(FigPlayer))

/** Player half-extents for collision (a 16×32×16 box). Pass these as
 *  mins/maxs to fig_clip_box/slide/ground. */
extern const fm_vec3_t FIG_PLAYER_MINS;
extern const fm_vec3_t FIG_PLAYER_MAXS;

/** Player event ids. The actor's FigActorEventFn dispatches on these. */
enum {
    FIG_EV_PLAYER_FOOTSTEP = 0x10,
};

/** Get the player state from an actor's state block. */
static inline FigPlayer *fig_player_of(FigActor *a)
{
    return (FigPlayer *)a->state;
}

/** Set the camera-relative movement basis. Call once per frame before
 *  fig_player_update so stick-up moves the player in the camera's forward
 *  direction. If never called, movement defaults to world axes (forward
 *  +Z, right +X). The y components are ignored (movement is on the XZ
 *  plane); pass camera forward/right projected to XZ. */
void fig_player_set_camera_basis(fm_vec3_t fwd, fm_vec3_t right);

/** One-frame locomotion update. Reads fig_input_get(port), integrates
 *  velocity against fig_clip_slide, probes ground with fig_clip_ground,
 *  advances the state machine, and posts FOOTSTEP events when due. The
 *  actor's transform is updated in place (pos + yaw). `port` is 1-based
 *  to match fig_input_get; pass 0 for the default. */
void fig_player_update(FigActor *self, int port, float dt);

#ifdef __cplusplus
}
#endif

#endif /* FIG_PLAYER_H */