/* SPDX-License-Identifier: MIT
 *
 * kiln_skel.h — skeletal animation. See CLAUDE.md's Phase B notes for what
 * was and wasn't carried over from OoT's animation system.
 *
 * ── A thin wrapper, not a reimplementation ─────────────────────────────
 * Tiny3D already ships T3DSkeleton (bone pose + fixed-point matrices) and
 * T3DAnim (a playhead into one of a model's named animation clips) — see
 * t3d/t3dskeleton.h and t3d/t3danim.h. This module does not re-invent that;
 * it packages the two-skeleton blending idiom Tiny3D's own
 * examples/08_animation demonstrates (one primary skeleton with matrices,
 * one pose-only clone for a second animation, blended by a live factor)
 * into a shape that fits KilnActor's per-instance state, and adds the
 * bookkeeping every caller would otherwise duplicate.
 *
 * ── Three slots, and why the third exists ───────────────────────────────
 * BASE and BLEND are the locomotion pair: idle<->walk<->run as a function of
 * speed, or a crossfade between any two clips. That is Tiny3D's idiom and it
 * stays the whole of what most characters need.
 *
 * OVERLAY is a third pose-only clone, blended over the result for a MASK of
 * bones. It exists because a one-shot over locomotion — swinging a sword
 * while running, waving while walking — cannot be expressed in two slots:
 * putting the attack in BLEND stops the legs. The masked blend is this
 * engine's own arithmetic (kiln_pose.h), host-checked by
 * nix/checks/kiln-pose.nix. Still no N-way blend tree: three slots is the
 * most a character on this console has needed, and each is 21 bones x 108
 * bytes of RAM for goblin.py's rig.
 *
 * ── Clips are cached, not recreated ──────────────────────────────────────
 * t3d_anim_create opens the clip's .sdata sidecar and mallocs; attaching an
 * existing T3DAnim to another skeleton just rebinds its targets. So every clip
 * a KilnSkel plays is created once, on first use, and kept until
 * kiln_skel_destroy — a state machine swapping clips every few frames costs an
 * attach, not a file open and a heap churn. A clip wanted by two slots at once
 * (Walk fading out of BASE while Walk fades into BLEND) gets a second
 * instance; KILN_SKEL_CLIPS_MAX bounds the total.
 *
 * ── Clip time is REAL time, and the goblin's clips are authored at 24 fps ──
 * A clip's length is whatever gltf_to_t3d read from the glTF, and Blender
 * exports keyframe times at the scene rate, which nothing in tools/blender
 * sets — so a "40-frame" Walk is 1.667 s, not 0.667 s. Read
 * kiln_skel_length() rather than deriving a duration from a frame count.
 *
 * ── Ownership: caller allocates, module fills in ───────────────────────
 * KilnSkel is a plain struct, same convention as KilnTransform in
 * kiln_engine.h: too large to embed inline in KilnActor's fixed state block
 * (see KILN_ACTOR_STATE_MAX), so a skinned actor's state holds an `KilnSkel*`
 * to one allocated at spawn time. `model` is borrowed, not owned; the caller
 * keeps it alive for exactly as long as any KilnSkel built from it is alive.
 * Clip NAMES are borrowed too — pass string literals.
 */
#ifndef KILN_SKEL_H
#define KILN_SKEL_H

#include <t3d/t3dmodel.h>
#include <t3d/t3dskeleton.h>
#include <t3d/t3danim.h>

#include "kiln_pose.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Cached clip instances per KilnSkel (see the file comment). */
#ifndef KILN_SKEL_CLIPS_MAX
#define KILN_SKEL_CLIPS_MAX 16
#endif

/** Bone rotation overrides applied per update (look-at, aim). */
#define KILN_SKEL_OVERRIDES_MAX 4

typedef enum {
    KILN_SKEL_BASE = 0,  /**< drives `skel`, the pose that gets drawn        */
    KILN_SKEL_BLEND,     /**< mixed into BASE by `blend_factor`              */
    KILN_SKEL_OVERLAY,   /**< mixed over the result, for `overlay_mask` bones */
    KILN_SKEL_SLOTS
} KilnSkelSlot;

typedef struct {
    const T3DModel *model; /**< borrowed; see the file comment */

    T3DSkeleton skel;       /**< BASE pose; this is what gets drawn */
    T3DSkeleton skel_blend; /**< BLEND pose, matrix-less */
    T3DSkeleton skel_over;  /**< OVERLAY pose, matrix-less, created on first use */
    bool has_skel_over;

    T3DAnim     clips[KILN_SKEL_CLIPS_MAX];
    const char *clip_names[KILN_SKEL_CLIPS_MAX];
    uint8_t     clip_count;
    int8_t      slot_clip[KILN_SKEL_SLOTS];   /**< index into clips, -1 = none */
    float       slot_speed[KILN_SKEL_SLOTS];

    float blend_factor;     /**< 0 = pure BASE, 1 = pure BLEND */
    float blend_target;     /**< where a crossfade is heading */
    float blend_rate;       /**< factor per second; 0 = not fading */

    uint32_t overlay_mask;  /**< bones the overlay touches; default all */
    float overlay_weight;   /**< current envelope, 0..1 */
    float overlay_fade;     /**< seconds in and out */
    bool  overlay_loop;
    bool  overlay_stopping;

    struct { int16_t bone; T3DQuat delta; } overrides[KILN_SKEL_OVERRIDES_MAX];
    uint8_t override_count;

    /* Kept for callers that read them. */
    bool has_anim;          /**< a BASE clip is attached */
    bool has_blend_anim;    /**< a BLEND clip is attached */
} KilnSkel;

/** Create the skeleton instances from `model`. Does not start playing
 *  anything — call kiln_skel_play before the first kiln_skel_update, or the
 *  bind pose is what draws. */
void kiln_skel_create(KilnSkel *sk, const T3DModel *model);

/** Frees the skeletons and every cached clip. */
void kiln_skel_destroy(KilnSkel *sk);

/** (Re)attach `name` to BASE, from its start. Safe to call every time a state
 *  machine wants a different clip. Does NOT reset bones the new clip leaves
 *  untouched to their bind pose; if that matters, t3d_skeleton_reset(&sk->skel)
 *  first. A hard cut — see kiln_skel_crossfade for the soft one. */
void kiln_skel_play(KilnSkel *sk, const char *name, bool loop);

/** As kiln_skel_play, for BLEND. NULL detaches it — after that,
 *  kiln_skel_update skips the blend and `skel` is driven by BASE alone. */
void kiln_skel_play_blend(KilnSkel *sk, const char *name, bool loop);

/** Mix factor for BLEND: 0 = pure BASE, 1 = pure BLEND, clamped to that range.
 *  Cancels any crossfade in progress. */
void kiln_skel_set_blend(KilnSkel *sk, float factor);

/** Fade to `name` over `seconds`, using whichever of BASE/BLEND currently
 *  carries LESS weight: the clip is attached there from its start and the
 *  factor ramps towards it. A no-op when `name` already holds the heavier
 *  slot. `seconds` <= 0 cuts. The lighter slot's pose is reset to bind before
 *  the attach, so a clip that keys fewer bones than the last one does not
 *  inherit its leftovers. */
void kiln_skel_crossfade(KilnSkel *sk, const char *name, bool loop, float seconds);

/** The clip name attached to `slot`, or NULL. */
const char *kiln_skel_clip(const KilnSkel *sk, KilnSkelSlot slot);

/** Playback rate for `slot` (1 = authored speed; negative is clamped to 0 by
 *  Tiny3D). Persists across clip swaps on that slot. */
void kiln_skel_set_speed(KilnSkel *sk, KilnSkelSlot slot, float speed);

/** Seconds into `slot`'s clip, and the clip's length. 0 when nothing is
 *  attached. */
float kiln_skel_time(const KilnSkel *sk, KilnSkelSlot slot);
float kiln_skel_length(const KilnSkel *sk, KilnSkelSlot slot);

/** Jump `slot` to a fraction 0..1 of its clip — for matching a run's stride to
 *  the walk it replaces. Moving FORWARD makes the next update read every
 *  keyframe up to that point from the cartridge, so call this on a clip swap,
 *  never per frame. */
void kiln_skel_set_phase(KilnSkel *sk, KilnSkelSlot slot, float phase);

/** Play `name` on OVERLAY, faded in and out over `fade_s`, over the bones in
 *  the current overlay mask. A one-shot fades out on its own ahead of its end:
 *  Tiny3D stops a finished clip WITHOUT applying its final pose
 *  (t3danim.c's rollover returns before sampling), so the envelope has to be
 *  at zero by then. A looping overlay runs until kiln_skel_overlay_stop. */
void kiln_skel_overlay(KilnSkel *sk, const char *name, bool loop, float fade_s);

/** Fade the overlay out over its fade time. */
void kiln_skel_overlay_stop(KilnSkel *sk);

/** True while the overlay carries any weight. */
bool kiln_skel_overlay_active(const KilnSkel *sk);

/** The bones the overlay drives. KILN_POSE_MASK_ALL by default. */
void kiln_skel_set_overlay_mask(KilnSkel *sk, uint32_t mask);

/** Index of bone `name`, or -1. Resolve by name: the exporter's bone order is
 *  not the authoring script's. */
int kiln_skel_bone(const KilnSkel *sk, const char *name);

/** Mask of bone `name` and everything below it (e.g. "torso" = torso, neck,
 *  head, arms). 0 if the bone does not exist. */
uint32_t kiln_skel_mask_bone(const KilnSkel *sk, const char *name);

/** Rotate `bone` by `local_delta` (in its parent's space, applied after its
 *  animated rotation) on the NEXT kiln_skel_update only. Call every frame the
 *  override should hold — a head look-at, a spine aim. Its children follow. */
void kiln_skel_bone_rotate(KilnSkel *sk, int bone, const T3DQuat *local_delta);

/** Advances every attached clip, applies the crossfade, the blend, the
 *  overlay and any bone overrides, and rebuilds `skel`'s matrices. Call once
 *  per frame, before kiln_skel_draw and before any bone query. */
void kiln_skel_update(KilnSkel *sk, float dt);

/** t3d_model_draw_skinned(model, &skel), plus t3d_skeleton_use for the
 *  buffered-skeleton case. Call inside the 3D pass after the actor's
 *  KilnTransform has been pushed. */
void kiln_skel_draw(const KilnSkel *sk);

/** Push `bone`'s current matrix, so what is drawn next rides that bone: a
 *  sword in the hand, a hat on the head. Nest it inside the actor's
 *  kiln_transform_push and pop it with kiln_transform_pop. Bone matrices are
 *  in MODEL units — the model's ×64 base scale is baked in and the actor's
 *  own scale applies on top — so author the prop at model scale, or push a
 *  KilnTransform offset after this to place and size it. */
void kiln_skel_bone_push(const KilnSkel *sk, int bone);

/** `bone`'s origin in model space (model units), as of the last update. */
T3DVec3 kiln_skel_bone_pos(const KilnSkel *sk, int bone);

/** True once BASE's clip has finished — never for a looping clip, and true if
 *  nothing is attached. What a state machine polls to chain one-shots. */
bool kiln_skel_is_done(const KilnSkel *sk);

#ifdef __cplusplus
}
#endif

#endif /* KILN_SKEL_H */
