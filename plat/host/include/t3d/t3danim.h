/* SPDX-License-Identifier: MIT
 *
 * plat/host/include/t3d/t3danim.h — the host's <t3d/t3danim.h>.
 * Types only; every entry point aborts. See t3dskeleton.h for why that is the
 * right shape rather than a no-op.
 */
#ifndef KILN_HOST_T3DANIM_H
#define KILN_HOST_T3DANIM_H

#include <stdint.h>
#include <stdbool.h>
#include <t3d/t3dmodel.h>
#include <t3d/t3dskeleton.h>

typedef struct {
    const T3DChunkAnim *animRef;
    float       time;
    float       speed;
    uint8_t     isPlaying;
    uint8_t     isLooping;
} T3DAnim;

T3DAnim t3d_anim_create(const struct T3DModel *model, const char *name);
void t3d_anim_destroy(T3DAnim *anim);
void t3d_anim_attach(T3DAnim *anim, const T3DSkeleton *skeleton);
void t3d_anim_update(T3DAnim *anim, float deltaTime);
void t3d_anim_set_looping(T3DAnim *anim, bool loop);
void t3d_anim_set_playing(T3DAnim *anim, bool play);
void t3d_anim_set_time(T3DAnim *anim, float time);
void t3d_anim_set_speed(T3DAnim *anim, float speed);
bool t3d_anim_is_playing(const T3DAnim *anim);

/* Inline on the console too (Tiny3D's t3danim.h), and plain reads. */
static inline float t3d_anim_get_time(const T3DAnim *anim) { return anim->time; }
static inline float t3d_anim_get_length(const T3DAnim *anim) { return anim->animRef->duration; }

#endif /* KILN_HOST_T3DANIM_H */
