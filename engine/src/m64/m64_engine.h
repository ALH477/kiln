/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_engine.h — the 3D half of the M64 engine.
 *
 * ── The two-layer model ────────────────────────────────────────────────
 * A frame is two distinct passes over the same framebuffer:
 *
 *   m64_frame_begin()      attach the framebuffer + Z-buffer
 *     m64_scene_draw()     3D: Tiny3D, perspective, lighting, depth-tested
 *     m64_gui_begin()      switch the RDP into a flat 2D mode
 *       ... 2D widgets ... GUI: orthographic, no depth, no lighting
 *     m64_gui_end()
 *   m64_frame_end()        present
 *
 * The split is not cosmetic. The RDP is a state machine, and 3D and 2D want
 * genuinely opposite configurations: the 3D pass needs the Z-buffer, shading
 * and perspective-correct texturing; the 2D pass needs all of that OFF or it
 * pays for depth compares on a HUD that can never be occluded, and risks the
 * HUD being z-rejected by geometry drawn in front of it. Keeping them as two
 * explicit passes with one transition makes the state change happen exactly
 * once per frame instead of per widget.
 *
 * See m64_gui.h for the 2D half.
 */
#ifndef M64_ENGINE_H
#define M64_ENGINE_H

#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A camera + lighting setup. One per viewport. */
typedef struct {
    T3DViewport viewport;

    fm_vec3_t cam_pos;
    fm_vec3_t cam_target;
    fm_vec3_t cam_up;

    float fov_deg;
    float near_z, far_z;

    uint8_t ambient[4];
    uint8_t light_color[4];
    fm_vec3_t light_dir;
    int light_count;

    color_t clear_color;
} M64Scene;

/** A model transform plus the fixed-point matrix the RSP actually consumes.
 *
 * Tiny3D's RSP microcode reads matrices as s16.16 fixed point from UNCACHED
 * memory. Holding both representations here means callers work in floats and
 * the conversion + upload happens in one place, which is also the only place
 * that has to remember the uncached allocation. */
typedef struct {
    fm_vec3_t pos;
    fm_vec3_t scale;
    fm_vec3_t rot_axis;
    float rot_angle;
    T3DMat4FP *mtx; /* uncached; owned by this struct */
} M64Transform;

/** Initialise display, rdpq and Tiny3D. Call once.
 *  `res` is a libdragon resolution_t, e.g. RESOLUTION_320x240. */
void m64_engine_init(resolution_t res);
void m64_engine_close(void);

/** Fill a scene with sane defaults: 85 degree FOV, one white directional
 *  light, dim ambient, camera looking at the origin. */
void m64_scene_init(M64Scene *s);

/** Recompute projection + view from the scene's camera fields. Call after
 *  moving the camera, before m64_scene_begin. */
void m64_scene_update(M64Scene *s);

/** Begin the 3D pass: attach the viewport, clear colour + depth, upload
 *  lights, and set the default 3D draw flags. */
void m64_scene_begin(M64Scene *s);

/** Frame bracket. m64_frame_begin attaches the framebuffer and Z-buffer;
 *  m64_frame_end presents it. */
void m64_frame_begin(void);
void m64_frame_end(void);

/** Transform helpers. m64_transform_init allocates the uncached matrix. */
void m64_transform_init(M64Transform *t);
void m64_transform_free(M64Transform *t);

/** Rebuild the fixed-point matrix from pos/scale/rot and push it. Pair with
 *  m64_transform_pop after drawing. */
void m64_transform_push(M64Transform *t);
void m64_transform_pop(void);

#ifdef __cplusplus
}
#endif

#endif /* M64_ENGINE_H */
