/* SPDX-License-Identifier: MIT
 *
 * kiln_skel.c — see kiln_skel.h for the model.
 */

#include "kiln_skel.h"

#include <string.h>

static int bone_count(const KilnSkel *sk)
{
    return sk->skel.skeletonRef->boneCount;
}

static T3DSkeleton *slot_skel(KilnSkel *sk, KilnSkelSlot slot)
{
    switch (slot) {
    case KILN_SKEL_BLEND:   return &sk->skel_blend;
    case KILN_SKEL_OVERLAY:
        if (!sk->has_skel_over) {
            sk->skel_over = t3d_skeleton_clone(&sk->skel, false);
            sk->has_skel_over = true;
        }
        return &sk->skel_over;
    default:                return &sk->skel;
    }
}

static T3DAnim *slot_anim(const KilnSkel *sk, KilnSkelSlot slot)
{
    const int k = sk->slot_clip[slot];
    return k < 0 ? NULL : (T3DAnim *)&sk->clips[k];
}

static bool clip_in_use(const KilnSkel *sk, int k)
{
    for (int s = 0; s < KILN_SKEL_SLOTS; s++)
        if (sk->slot_clip[s] == k) return true;
    return false;
}

/* A cached instance of `name` that no slot is using, created if there is
 * none. `except` is the slot about to receive it, whose current clip is being
 * replaced and so counts as free. */
static int acquire_clip(KilnSkel *sk, const char *name, KilnSkelSlot except)
{
    for (int k = 0; k < sk->clip_count; k++) {
        if (strcmp(sk->clip_names[k], name) != 0) continue;
        if (sk->slot_clip[except] == k || !clip_in_use(sk, k)) return k;
    }
    assertf(sk->clip_count < KILN_SKEL_CLIPS_MAX,
            "kiln_skel: more than %d cached clips (raise KILN_SKEL_CLIPS_MAX)", KILN_SKEL_CLIPS_MAX);
    const int k = sk->clip_count++;
    sk->clips[k] = t3d_anim_create(sk->model, name);
    sk->clip_names[k] = name;
    return k;
}

static void attach(KilnSkel *sk, KilnSkelSlot slot, const char *name, bool loop, bool reset)
{
    const int k = acquire_clip(sk, name, slot);
    T3DSkeleton *dst = slot_skel(sk, slot);
    if (reset) t3d_skeleton_reset(dst);

    T3DAnim *a = &sk->clips[k];
    t3d_anim_attach(a, dst);           /* rebinds targets and rewinds the stream */
    a->time = 0.0f;                    /* attach rewinds the file, not the clock */
    t3d_anim_set_looping(a, loop);
    t3d_anim_set_playing(a, true);
    t3d_anim_set_speed(a, sk->slot_speed[slot]);
    sk->slot_clip[slot] = (int8_t)k;

    sk->has_anim = sk->slot_clip[KILN_SKEL_BASE] >= 0;
    sk->has_blend_anim = sk->slot_clip[KILN_SKEL_BLEND] >= 0;
}

void kiln_skel_create(KilnSkel *sk, const T3DModel *model)
{
    memset(sk, 0, sizeof(*sk));
    sk->model = model;
    sk->skel = t3d_skeleton_create(model);
    sk->skel_blend = t3d_skeleton_clone(&sk->skel, false);
    for (int s = 0; s < KILN_SKEL_SLOTS; s++) {
        sk->slot_clip[s] = -1;
        sk->slot_speed[s] = 1.0f;
    }
    sk->overlay_mask = KILN_POSE_MASK_ALL;
}

void kiln_skel_destroy(KilnSkel *sk)
{
    for (int k = 0; k < sk->clip_count; k++) t3d_anim_destroy(&sk->clips[k]);
    sk->clip_count = 0;
    for (int s = 0; s < KILN_SKEL_SLOTS; s++) sk->slot_clip[s] = -1;
    sk->has_anim = sk->has_blend_anim = false;
    t3d_skeleton_destroy(&sk->skel);
    t3d_skeleton_destroy(&sk->skel_blend);
    if (sk->has_skel_over) t3d_skeleton_destroy(&sk->skel_over);
    sk->has_skel_over = false;
}

void kiln_skel_play(KilnSkel *sk, const char *name, bool loop)
{
    attach(sk, KILN_SKEL_BASE, name, loop, false);
}

void kiln_skel_play_blend(KilnSkel *sk, const char *name, bool loop)
{
    if (!name) {
        sk->slot_clip[KILN_SKEL_BLEND] = -1;
        sk->has_blend_anim = false;
        return;
    }
    attach(sk, KILN_SKEL_BLEND, name, loop, false);
}

void kiln_skel_set_blend(KilnSkel *sk, float factor)
{
    if (factor < 0.0f) factor = 0.0f;
    if (factor > 1.0f) factor = 1.0f;
    sk->blend_factor = factor;
    sk->blend_target = factor;
    sk->blend_rate = 0.0f;
}

const char *kiln_skel_clip(const KilnSkel *sk, KilnSkelSlot slot)
{
    const int k = sk->slot_clip[slot];
    return k < 0 ? NULL : sk->clip_names[k];
}

void kiln_skel_crossfade(KilnSkel *sk, const char *name, bool loop, float seconds)
{
    /* The heavier side is what is on screen. Heading towards BLEND counts as
     * BLEND being heavier already, so two crossfades in a row alternate. */
    const bool blend_heavy = sk->blend_rate != 0.0f ? sk->blend_target >= 0.5f
                                                    : sk->blend_factor >= 0.5f;
    const KilnSkelSlot heavy = blend_heavy ? KILN_SKEL_BLEND : KILN_SKEL_BASE;
    const KilnSkelSlot light = blend_heavy ? KILN_SKEL_BASE : KILN_SKEL_BLEND;

    const char *cur = kiln_skel_clip(sk, heavy);
    if (cur && strcmp(cur, name) == 0) return;

    /* Never reset `skel` itself: BASE is also the drawn pose, and resetting it
     * mid-fade would flash the bind pose for a frame. */
    attach(sk, light, name, loop, light != KILN_SKEL_BASE);
    sk->blend_target = light == KILN_SKEL_BLEND ? 1.0f : 0.0f;
    if (seconds <= 0.0f) {
        sk->blend_factor = sk->blend_target;
        sk->blend_rate = 0.0f;
    } else {
        sk->blend_rate = 1.0f / seconds;
    }
}

void kiln_skel_set_speed(KilnSkel *sk, KilnSkelSlot slot, float speed)
{
    sk->slot_speed[slot] = speed;
    T3DAnim *a = slot_anim(sk, slot);
    if (a) t3d_anim_set_speed(a, speed);
}

float kiln_skel_time(const KilnSkel *sk, KilnSkelSlot slot)
{
    const T3DAnim *a = slot_anim(sk, slot);
    return a ? t3d_anim_get_time(a) : 0.0f;
}

float kiln_skel_length(const KilnSkel *sk, KilnSkelSlot slot)
{
    const T3DAnim *a = slot_anim(sk, slot);
    return a ? t3d_anim_get_length(a) : 0.0f;
}

void kiln_skel_set_phase(KilnSkel *sk, KilnSkelSlot slot, float phase)
{
    T3DAnim *a = slot_anim(sk, slot);
    if (!a) return;
    phase -= (float)(int)phase;
    if (phase < 0.0f) phase += 1.0f;
    t3d_anim_set_time(a, phase * t3d_anim_get_length(a));
}

void kiln_skel_overlay(KilnSkel *sk, const char *name, bool loop, float fade_s)
{
    attach(sk, KILN_SKEL_OVERLAY, name, loop, true);
    sk->overlay_fade = fade_s > 0.0f ? fade_s : 0.0f;
    sk->overlay_loop = loop;
    sk->overlay_stopping = false;
    if (sk->overlay_fade == 0.0f) sk->overlay_weight = 1.0f;
}

void kiln_skel_overlay_stop(KilnSkel *sk)
{
    if (sk->slot_clip[KILN_SKEL_OVERLAY] >= 0) sk->overlay_stopping = true;
}

bool kiln_skel_overlay_active(const KilnSkel *sk)
{
    return sk->slot_clip[KILN_SKEL_OVERLAY] >= 0;
}

void kiln_skel_set_overlay_mask(KilnSkel *sk, uint32_t mask)
{
    sk->overlay_mask = mask;
}

int kiln_skel_bone(const KilnSkel *sk, const char *name)
{
    const int n = bone_count(sk);
    for (int i = 0; i < n; i++)
        if (strcmp(sk->skel.skeletonRef->bones[i].name, name) == 0) return i;
    return -1;
}

uint32_t kiln_skel_mask_bone(const KilnSkel *sk, const char *name)
{
    const int root = kiln_skel_bone(sk, name);
    const int n = bone_count(sk);
    assertf(n <= KILN_POSE_MAX_BONES, "kiln_skel: %d bones, masks cover %d", n, KILN_POSE_MAX_BONES);
    uint16_t depth[KILN_POSE_MAX_BONES];
    for (int i = 0; i < n; i++) depth[i] = sk->skel.skeletonRef->bones[i].depth;
    return kiln_pose_subtree_mask(depth, n, root);
}

void kiln_skel_bone_rotate(KilnSkel *sk, int bone, const T3DQuat *local_delta)
{
    if (bone < 0 || sk->override_count >= KILN_SKEL_OVERRIDES_MAX) return;
    sk->overrides[sk->override_count].bone = (int16_t)bone;
    sk->overrides[sk->override_count].delta = *local_delta;
    sk->override_count++;
}

static float approach(float v, float target, float step)
{
    if (v < target) return v + step > target ? target : v + step;
    return v - step < target ? target : v - step;
}

void kiln_skel_update(KilnSkel *sk, float dt)
{
    const int n = bone_count(sk);

    if (sk->blend_rate != 0.0f) {
        sk->blend_factor = approach(sk->blend_factor, sk->blend_target, sk->blend_rate * dt);
        if (sk->blend_factor == sk->blend_target) sk->blend_rate = 0.0f;
    }

    T3DAnim *base = slot_anim(sk, KILN_SKEL_BASE);
    T3DAnim *blend = slot_anim(sk, KILN_SKEL_BLEND);
    if (base) t3d_anim_update(base, dt);
    if (blend) {
        t3d_anim_update(blend, dt);
        t3d_skeleton_blend(&sk->skel, &sk->skel, &sk->skel_blend, sk->blend_factor);
    }

    T3DAnim *over = slot_anim(sk, KILN_SKEL_OVERLAY);
    if (over) {
        t3d_anim_update(over, dt);

        /* The envelope: up over `fade` from the start, down over `fade`
         * before the end of a one-shot, down from wherever it is on a stop. */
        const float len = t3d_anim_get_length(over);
        const float t = t3d_anim_get_time(over);
        float target = 1.0f;
        if (sk->overlay_stopping) target = 0.0f;
        else if (!sk->overlay_loop && (t >= len - sk->overlay_fade || !t3d_anim_is_playing(over)))
            target = 0.0f;
        const float step = sk->overlay_fade > 0.0f ? dt / sk->overlay_fade : 1.0f;
        sk->overlay_weight = approach(sk->overlay_weight, target, step);

        if (sk->overlay_weight > 0.0f)
            kiln_pose_blend_masked(sk->skel.bones, sk->skel.bones, sk->skel_over.bones,
                                   n, sk->overlay_mask, sk->overlay_weight);
        else if (target == 0.0f)
            sk->slot_clip[KILN_SKEL_OVERLAY] = -1;   /* faded out: detach */
    } else {
        sk->overlay_weight = 0.0f;
    }

    for (int i = 0; i < sk->override_count; i++) {
        T3DBone *b = &sk->skel.bones[sk->overrides[i].bone];
        kiln_quat_mul(&b->rotation, &b->rotation, &sk->overrides[i].delta);
        b->hasChanged = 1;
    }
    sk->override_count = 0;

    t3d_skeleton_update(&sk->skel);
}

void kiln_skel_draw(const KilnSkel *sk)
{
    t3d_skeleton_use(&sk->skel);
    t3d_model_draw_skinned(sk->model, &sk->skel);
}

void kiln_skel_bone_push(const KilnSkel *sk, int bone)
{
    assertf(bone >= 0 && bone < bone_count(sk), "kiln_skel_bone_push: bone %d", bone);
    t3d_matrix_push(&sk->skel.boneMatricesFP[sk->skel.currentBufferIdx * bone_count(sk) + bone]);
}

T3DVec3 kiln_skel_bone_pos(const KilnSkel *sk, int bone)
{
    const T3DMat4 *m = &sk->skel.bones[bone].matrix;
    return (T3DVec3){{ m->m[3][0], m->m[3][1], m->m[3][2] }};
}

bool kiln_skel_is_done(const KilnSkel *sk)
{
    const T3DAnim *a = slot_anim(sk, KILN_SKEL_BASE);
    return !a || !t3d_anim_is_playing(a);
}
