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
 * play/swap bookkeeping (destroying the old T3DAnim, attaching the new one)
 * that every caller would otherwise duplicate.
 *
 * ── Two slots, not a general N-way blend tree ──────────────────────────
 * OoT's real animation system supports layered blending across many
 * channels (upper/lower body split, partial-bone masks). Two slots blended
 * by one scalar factor is Tiny3D's own idiom (see the file comment above)
 * and covers the common case a locomotion blend needs — idle<->walk<->run
 * as a function of speed, or a hard cut by driving the factor to 0 or 1 —
 * without a blend tree neither this engine nor a 4 MB console needs. A
 * game wanting N-way blending composes multiple KilnSkel-like pairs by hand;
 * this module does not try to anticipate that.
 *
 * ── Ownership: caller allocates, module fills in ───────────────────────
 * KilnSkel is a plain struct, same convention as KilnTransform in
 * kiln_engine.h: too large to embed inline in KilnActor's fixed state block
 * (see KILN_ACTOR_STATE_MAX), so a skinned actor's state holds an `KilnSkel*`
 * to one allocated at spawn time (or shared across instances of the same
 * type, if they always show the same pose — a game with many identical
 * skinned enemies should weigh that against the memory cost of one
 * KilnSkel per instance). `model` is borrowed, not owned; the caller keeps
 * it alive (and frees it with t3d_model_free) for exactly as long as any
 * KilnSkel built from it is alive.
 */
#ifndef KILN_SKEL_H
#define KILN_SKEL_H

#include <t3d/t3dmodel.h>
#include <t3d/t3dskeleton.h>
#include <t3d/t3danim.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const T3DModel *model; /**< borrowed; see the file comment */

    T3DSkeleton skel;       /**< primary pose; this is what gets drawn */
    T3DSkeleton skel_blend; /**< secondary pose, matrix-less (see Tiny3D's
                              *   t3d_skeleton_clone(..., false)) — exists
                              *   only to blend into `skel`, never drawn
                              *   directly */
    T3DAnim anim;           /**< drives `skel`; valid once has_anim */
    T3DAnim anim_blend;     /**< drives `skel_blend`; valid once has_blend_anim */

    bool has_anim;
    bool has_blend_anim;
    float blend_factor;     /**< 0 = pure `anim`, 1 = pure `anim_blend` */
} KilnSkel;

/** Create both skeleton instances from `model`. Does not start playing
 *  anything — call kiln_skel_play before the first kiln_skel_update, or the
 *  bind pose is what draws. */
void kiln_skel_create(KilnSkel *sk, const T3DModel *model);

/** Frees both skeletons and whichever animations are attached. Safe to
 *  call whether or not kiln_skel_play/play_blend was ever called. */
void kiln_skel_destroy(KilnSkel *sk);

/** (Re)attach `name` as the primary animation. Destroys whatever was
 *  previously attached first — safe to call every time a state machine
 *  wants a different clip, not just once at spawn. Does NOT reset bones a
 *  new clip leaves untouched to their bind pose; if that matters for a
 *  specific model (a clip that only animates some bones), reset via
 *  t3d_skeleton_reset(&sk->skel) before playing it. */
void kiln_skel_play(KilnSkel *sk, const char *name, bool loop);

/** As kiln_skel_play, but for the secondary blend slot. Pass NULL to detach
 *  it — after that, kiln_skel_update skips the blend and `skel` is driven
 *  by `anim` alone regardless of blend_factor. */
void kiln_skel_play_blend(KilnSkel *sk, const char *name, bool loop);

/** Mix factor for the blend slot: 0 = pure primary, 1 = pure blend, clamped
 *  to that range. No-op if no blend animation is attached. */
void kiln_skel_set_blend(KilnSkel *sk, float factor);

/** Advances whichever animation(s) are attached, blends if a blend clip is
 *  attached, and recomputes `skel`'s bone matrices. Call once per frame,
 *  before kiln_skel_draw. */
void kiln_skel_update(KilnSkel *sk, float dt);

/** t3d_model_draw_skinned(model, &skel), plus t3d_skeleton_use for the
 *  buffered-skeleton case. Call inside the 3D pass after the actor's
 *  KilnTransform has been pushed (same convention as any other draw call
 *  between kiln_transform_push/pop). */
void kiln_skel_draw(const KilnSkel *sk);

/** True once the primary clip has finished — the inverse of Tiny3D's own
 *  t3d_anim_is_playing: never true for a looping clip (it runs forever
 *  once started), true once a non-looping clip reaches its last frame.
 *  True if no clip is attached (nothing to wait for). This is what a state
 *  machine polls to chain one-shot clips — e.g. "cut to sit_type once
 *  sit_down finishes" — rather than guessing a fixed frame count. */
bool kiln_skel_is_done(const KilnSkel *sk);

#ifdef __cplusplus
}
#endif

#endif /* KILN_SKEL_H */
