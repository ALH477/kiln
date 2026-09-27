/* SPDX-License-Identifier: MIT
 *
 * plat/host/include/t3d/t3dskeleton.h — the host's <t3d/t3dskeleton.h>.
 *
 * Every entry point here is IMPLEMENTED, and the implementation is Tiny3D's
 * own, copied rather than approximated (host_t3dmodel.c carries it). This file
 * used to be types with a loud abort behind each one, and that was the right
 * shape for as long as it was true; the note it carried is worth keeping
 * because it is the reason the replacement had to be exact rather than
 * convenient:
 *
 *   "A silent no-op would be much worse. CLAUDE.md records that a skinned
 *   model drawn at the wrong origin cost this project a full pass of camera
 *   retuning, because 'cameras aimed at him photographing empty room' reads as
 *   a framing problem."
 *
 * So the bind pose the host computes is the bind pose the console computes:
 * the same 'S' chunk, the same t3d_mat4_from_srt, the same parent composition,
 * the same fixed-point quantisation through t3d_mat4_to_fixed. A model at the
 * wrong origin natively means it is at the wrong origin on console too.
 *
 * ── What is still staged ───────────────────────────────────────────────
 * ANIMATION, and only its keyframes. A clip's keyframe stream lives in a
 * .sdata sidecar beside the .t3dm, which the host does not read yet, so
 * t3d_anim_update keeps the console's clock exactly (time, speed, loop wrap,
 * the non-looping stop) and does not pose bones. It says so once per clip.
 * A skinned character therefore STANDS in its bind pose while every piece of
 * timing around it — fig_skel_set_phase, the overlay fade, fig_skel_done —
 * behaves as it will on hardware. That is deliberate: sequencing is what the
 * native loop is for, and it is testable now.
 *
 * The layouts are the console's, copied from Tiny3D's t3dskeleton.h. That was
 * already necessary when nothing here ran, because fig_skel reads members —
 * bone rotations for its masked overlay blend (kiln_pose.h, which
 * nix/checks/kiln-pose.nix runs natively over exactly this T3DBone), and the
 * skeleton reference for bone names and depths. Now that the matrices are
 * computed here too, a host layout of its own would let that arithmetic be
 * checked against a struct the console does not have.
 */
#ifndef FIG_HOST_T3DSKELETON_H
#define FIG_HOST_T3DSKELETON_H

#include <stdint.h>
#include <t3d/t3d.h>
#include <t3d/t3dmodel.h>

typedef struct {
    T3DMat4 matrix;
    T3DVec3 scale;
    T3DQuat rotation;
    T3DVec3 position;
    int32_t hasChanged;
} T3DBone;

typedef struct T3DSkeleton_s {
    T3DBone   *bones;
    T3DMat4FP *boneMatricesFP;
    uint8_t    bufferCount;
    uint8_t    currentBufferIdx;
    const T3DChunkSkeleton *skeletonRef;
} T3DSkeleton;

T3DSkeleton t3d_skeleton_create(const struct T3DModel *model);
T3DSkeleton t3d_skeleton_create_buffered(const struct T3DModel *model, int bufferCount);
T3DSkeleton t3d_skeleton_clone(const T3DSkeleton *skel, bool useMatrices);
void t3d_skeleton_destroy(T3DSkeleton *skel);
void t3d_skeleton_reset(T3DSkeleton *skel);
void t3d_skeleton_update(T3DSkeleton *skel);
void t3d_skeleton_use(const T3DSkeleton *skel);
void t3d_skeleton_blend(const T3DSkeleton *out, const T3DSkeleton *a,
                        const T3DSkeleton *b, float factor);

#endif /* FIG_HOST_T3DSKELETON_H */
