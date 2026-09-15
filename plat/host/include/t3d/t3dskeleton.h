/* SPDX-License-Identifier: MIT
 *
 * plat/host/include/t3d/t3dskeleton.h — the host's <t3d/t3dskeleton.h>.
 *
 * Types only, with every entry point a loud abort. Skinning is not
 * implemented in the host 3D pass, and this is the shape that says so
 * usefully: the modules that reference these (kiln_skel, kiln_vanim,
 * kiln_crater, kiln_splash) COMPILE and LINK, so they get -Werror and appear
 * honestly in HOST_MODULES, while a call site fails in a way nobody can read
 * as working.
 *
 * A silent no-op would be much worse. CLAUDE.md records that a skinned model
 * drawn at the wrong origin cost this project a full pass of camera retuning,
 * because "cameras aimed at him photographing empty room" reads as a framing
 * problem. A skeleton that silently did nothing would look exactly like that.
 *
 * The layouts are the console's, copied from Tiny3D's t3dskeleton.h, because
 * kiln_skel now reads members — bone rotations for its masked overlay blend
 * (kiln_pose.h, which nix/checks/kiln-pose.nix runs natively over exactly
 * this T3DBone), and the skeleton reference for bone names and depths. A host
 * layout of its own would let that arithmetic be checked against a struct the
 * console does not have.
 */
#ifndef KILN_HOST_T3DSKELETON_H
#define KILN_HOST_T3DSKELETON_H

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

#endif /* KILN_HOST_T3DSKELETON_H */
