/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_skel.c — see m64_skel.h for the model.
 */

#include "m64_skel.h"

void m64_skel_create(M64Skel *sk, const T3DModel *model)
{
    sk->model = model;
    sk->skel = t3d_skeleton_create(model);
    sk->skel_blend = t3d_skeleton_clone(&sk->skel, false);
    sk->anim = (T3DAnim){ 0 };
    sk->anim_blend = (T3DAnim){ 0 };
    sk->has_anim = false;
    sk->has_blend_anim = false;
    sk->blend_factor = 0.0f;
}

void m64_skel_destroy(M64Skel *sk)
{
    if (sk->has_anim) t3d_anim_destroy(&sk->anim);
    if (sk->has_blend_anim) t3d_anim_destroy(&sk->anim_blend);
    sk->has_anim = false;
    sk->has_blend_anim = false;
    t3d_skeleton_destroy(&sk->skel);
    t3d_skeleton_destroy(&sk->skel_blend);
}

void m64_skel_play(M64Skel *sk, const char *name, bool loop)
{
    if (sk->has_anim) t3d_anim_destroy(&sk->anim);
    sk->anim = t3d_anim_create(sk->model, name);
    t3d_anim_attach(&sk->anim, &sk->skel);
    t3d_anim_set_looping(&sk->anim, loop);
    sk->has_anim = true;
}

void m64_skel_play_blend(M64Skel *sk, const char *name, bool loop)
{
    if (sk->has_blend_anim) t3d_anim_destroy(&sk->anim_blend);
    if (!name) {
        sk->has_blend_anim = false;
        return;
    }
    sk->anim_blend = t3d_anim_create(sk->model, name);
    t3d_anim_attach(&sk->anim_blend, &sk->skel_blend);
    t3d_anim_set_looping(&sk->anim_blend, loop);
    sk->has_blend_anim = true;
}

void m64_skel_set_blend(M64Skel *sk, float factor)
{
    if (factor < 0.0f) factor = 0.0f;
    if (factor > 1.0f) factor = 1.0f;
    sk->blend_factor = factor;
}

void m64_skel_update(M64Skel *sk, float dt)
{
    if (!sk->has_anim) return;

    t3d_anim_update(&sk->anim, dt);
    if (sk->has_blend_anim) {
        t3d_anim_update(&sk->anim_blend, dt);
        t3d_skeleton_blend(&sk->skel, &sk->skel, &sk->skel_blend, sk->blend_factor);
    }
    t3d_skeleton_update(&sk->skel);
}

void m64_skel_draw(const M64Skel *sk)
{
    t3d_skeleton_use(&sk->skel);
    t3d_model_draw_skinned(sk->model, &sk->skel);
}

bool m64_skel_is_done(const M64Skel *sk)
{
    if (!sk->has_anim) return true;
    return !t3d_anim_is_playing(&sk->anim);
}
