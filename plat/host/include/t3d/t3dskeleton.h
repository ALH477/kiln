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
 * The engine reads no member of either type — verified across engine/src/kiln
 * and every downstream game built on it so far — so the layouts are the
 * host's own.
 */
#ifndef KILN_HOST_T3DSKELETON_H
#define KILN_HOST_T3DSKELETON_H

#include <stdint.h>
#include <t3d/t3d.h>

typedef struct T3DSkeleton_s {
    void      *bones;
    T3DMat4FP *boneMatricesFP;
    uint16_t   boneCount;
    uint8_t    currentBufferIdx;
    const void *skeletonRef;
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
