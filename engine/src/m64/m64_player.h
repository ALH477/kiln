/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_player.h — the player locomotion state machine. A helper, not an
 * actor profile: the player IS an actor (category PLAYER), and its profile
 * update/draw call into m64_player_* which owns the state machine. This
 * matches the plan's "player glue" division — the actor system stays
 * generic, the player-specific behaviour lives in its own module.
 *
 * ── State machine ─────────────────────────────────────────────────────
 * IDLE → WALK (stick > deadzone) → RUN (hold R or full tilt)
 * IDLE/WALK/RUN → ROLL (L-tap; brief invuln + small forward lunge)
 * IDLE/WALK/RUN → ATTACK (A-tap; brief hitbox in front)
 * any grounded → JUMP (B-tap; upward velocity)
 * JUMP → FALL (vy < 0) → grounded (m64_clip_ground hit) → IDLE/WALK
 *
 * Each state has its own per-frame update; transitions are guarded by
 * input edges (m64_input_pressed) and world state (m64_clip_ground). The
 * full machine is in m64_player_update, dispatched on M64PlayerState.
 *
 * ── Footstep events ───────────────────────────────────────────────────
 * While walking/running on ground, m64_player_update emits a FOOTSTEP
 * event to `self` (via m64_event_post) at a cadence proportional to speed;
 * the actor's M64ActorEventFn (if registered) can play the surface's SFX.
 * The demo in examples/oot-demo wires this to m64_surface_play_footstep.
 *
 * ── Why not a profile ─────────────────────────────────────────────────
 * A game may have multiple player-controlled actors (co-op, mount+dismount)
 * where the locomotion state machine is shared but the actor profile
 * differs. Keeping the state machine in a module that takes an M64Actor*
 * makes that a copy, not a fork. The actor's `state` block holds the
 * M64PlayerState struct (see m64_player_state_size for the asserted size).
 */
#ifndef M64_PLAYER_H
#define M64_PLAYER_H

#include <t3d/t3dmath.h>
#include "m64_actor.h"
#include "m64_input.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    M64_PLAYER_IDLE = 0,
    M64_PLAYER_WALK,
    M64_PLAYER_RUN,
    M64_PLAYER_ROLL,
    M64_PLAYER_ATTACK,
    M64_PLAYER_JUMP,
    M64_PLAYER_FALL,
} M64PlayerState;

typedef struct {
    M64PlayerState state;
    fm_vec3_t vel;
    float    yaw;        /* facing direction the locomotion drives        */
    float    state_t;    /* seconds in the current state                  */
    float    step_cd;    /* seconds until next FOOTSTEP event             */
    uint8_t  on_ground;  /* 1 if m64_clip_ground hit last frame           */
    uint8_t  last_surf;  /* hitsurface underfoot last frame               */
} M64Player;

/** Asserted-against-M64_ACTOR_STATE_MAX size, so a profile can set
 *  state_size = M64_PLAYER_STATE_SIZE at init. */
#define M64_PLAYER_STATE_SIZE (sizeof(M64Player))

/** Player half-extents for collision (a 16×32×16 box). Pass these as
 *  mins/maxs to m64_clip_box/slide/ground. */
extern const fm_vec3_t M64_PLAYER_MINS;
extern const fm_vec3_t M64_PLAYER_MAXS;

/** Player event ids. The actor's M64ActorEventFn dispatches on these. */
enum {
    M64_EV_PLAYER_FOOTSTEP = 0x10,
};

/** Get the player state from an actor's state block. */
static inline M64Player *m64_player_of(M64Actor *a)
{
    return (M64Player *)a->state;
}

/** Set the camera-relative movement basis. Call once per frame before
 *  m64_player_update so stick-up moves the player in the camera's forward
 *  direction. If never called, movement defaults to world axes (forward
 *  +Z, right +X). The y components are ignored (movement is on the XZ
 *  plane); pass camera forward/right projected to XZ. */
void m64_player_set_camera_basis(fm_vec3_t fwd, fm_vec3_t right);

/** One-frame locomotion update. Reads m64_input_get(port), integrates
 *  velocity against m64_clip_slide, probes ground with m64_clip_ground,
 *  advances the state machine, and posts FOOTSTEP events when due. The
 *  actor's transform is updated in place (pos + yaw). `port` is 1-based
 *  to match m64_input_get; pass 0 for the default. */
void m64_player_update(M64Actor *self, int port, float dt);

#ifdef __cplusplus
}
#endif

#endif /* M64_PLAYER_H */