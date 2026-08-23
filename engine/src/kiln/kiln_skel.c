/* SPDX-License-Identifier: MIT
 *
 * kiln_skel.c — see kiln_skel.h for the model.
 */

#include "kiln_skel.h"

void kiln_skel_create(KilnSkel *sk, const T3DModel *model)
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

void kiln_skel_destroy(KilnSkel *sk)
{
    if (sk->has_anim) t3d_anim_destroy(&sk->anim);
    if (sk->has_blend_anim) t3d_anim_destroy(&sk->anim_blend);
    sk->has_anim = false;
    sk->has_blend_anim = false;
    t3d_skeleton_destroy(&sk->skel);
    t3d_skeleton_destroy(&sk->skel_blend);
}

void kiln_skel_play(KilnSkel *sk, const char *name, bool loop)
{
    if (sk->has_anim) t3d_anim_destroy(&sk->anim);
    sk->anim = t3d_anim_create(sk->model, name);
    t3d_anim_attach(&sk->anim, &sk->skel);
    t3d_anim_set_looping(&sk->anim, loop);
    sk->has_anim = true;
}

void kiln_skel_play_blend(KilnSkel *sk, const char *name, bool loop)
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

void kiln_skel_set_blend(KilnSkel *sk, float factor)
{
    if (factor < 0.0f) factor = 0.0f;
    if (factor > 1.0f) factor = 1.0f;
    sk->blend_factor = factor;
}

void kiln_skel_update(KilnSkel *sk, float dt)
{
    if (!sk->has_anim) return;

    t3d_anim_update(&sk->anim, dt);
    if (sk->has_blend_anim) {
        t3d_anim_update(&sk->anim_blend, dt);
        t3d_skeleton_blend(&sk->skel, &sk->skel, &sk->skel_blend, sk->blend_factor);
    }
    t3d_skeleton_update(&sk->skel);
}

void kiln_skel_draw(const KilnSkel *sk)
{
    t3d_skeleton_use(&sk->skel);
    t3d_model_draw_skinned(sk->model, &sk->skel);
}

bool kiln_skel_is_done(const KilnSkel *sk)
{
    if (!sk->has_anim) return true;
    return !t3d_anim_is_playing(&sk->anim);
}
