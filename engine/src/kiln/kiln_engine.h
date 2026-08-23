/* SPDX-License-Identifier: MIT
 *
 * kiln_engine.h — the 3D half of the Kiln engine.
 *
 * ── The two-layer model ────────────────────────────────────────────────
 * A frame is two distinct passes over the same framebuffer:
 *
 *   kiln_frame_begin()      attach the framebuffer + Z-buffer
 *     kiln_scene_draw()     3D: Tiny3D, perspective, lighting, depth-tested
 *     kiln_gui_begin()      switch the RDP into a flat 2D mode
 *       ... 2D widgets ... GUI: orthographic, no depth, no lighting
 *     kiln_gui_end()
 *   kiln_frame_end()        present
 *
 * The split is not cosmetic. The RDP is a state machine, and 3D and 2D want
 * genuinely opposite configurations: the 3D pass needs the Z-buffer, shading
 * and perspective-correct texturing; the 2D pass needs all of that OFF or it
 * pays for depth compares on a HUD that can never be occluded, and risks the
 * HUD being z-rejected by geometry drawn in front of it. Keeping them as two
 * explicit passes with one transition makes the state change happen exactly
 * once per frame instead of per widget.
 *
 * See kiln_gui.h for the 2D half.
 */
#ifndef KILN_ENGINE_H
#define KILN_ENGINE_H

#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Directional lights a scene can carry.
 *
 *  Tiny3D allows 7 (t3d.h: "You can set up to 7 directional lights"); 4 is
 *  what fits a scene's worth of intent — a key, a fill, and two spare for a
 *  practical light or a second bounce — without making the upload loop in
 *  kiln_scene_begin longer than it needs to be. Raise it if a scene really
 *  wants more; nothing below assumes the number is 4. */
#define KILN_SCENE_MAX_LIGHTS 4

/** A camera + lighting setup. One per viewport.
 *
 *  ── On the light array ─────────────────────────────────────────────────
 *  `light_color`/`light_dir` ARE light 0 — the single-light scenes that
 *  predate this array set those two fields and leave `light_count` at 1,
 *  and behave exactly as they did. A scene that wants a key-plus-fill rig
 *  (a night exterior is the motivating case: one light alone leaves every
 *  shadowed face at ambient, which reads as missing geometry rather than as
 *  darkness) fills `lights[1..]` and raises `light_count`.
 *
 *  Before the array existed, kiln_scene_begin uploaded ONLY light 0 while
 *  still calling t3d_light_set_count(light_count) — so a scene that set
 *  light_count = 2 got one real light and one made of whatever was in the
 *  RSP's light slots. That is why this is an array rather than a second
 *  ad-hoc pair of fields. */
typedef struct {
    uint8_t   color[4];
    fm_vec3_t dir;
} KilnLight;

typedef struct {
    T3DViewport viewport;

    fm_vec3_t cam_pos;
    fm_vec3_t cam_target;
    fm_vec3_t cam_up;

    float fov_deg;
    float near_z, far_z;

    uint8_t ambient[4];
    /* Light 0. Kept as named fields rather than folded into `lights` so
     * every existing caller compiles and reads unchanged. */
    uint8_t light_color[4];
    fm_vec3_t light_dir;
    /* Lights 1..KILN_SCENE_MAX_LIGHTS-1. Index i here is light i+1. */
    KilnLight lights[KILN_SCENE_MAX_LIGHTS - 1];
    int light_count;

    /* ── Fog ────────────────────────────────────────────────────────────
     * Off by default. Fog is per-pixel in the blender and costs nothing
     * extra for a scene that already fogs, but it is a scene-wide decision:
     * enabling it changes how every model in the pass is combined, so it
     * belongs here rather than being set behind the scene's back. */
    uint8_t fog_enabled;
    color_t fog_color;
    float   fog_near, fog_far;

    color_t clear_color;
} KilnScene;

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
} KilnTransform;

/** Initialise display, rdpq and Tiny3D. Call once.
 *  `res` is a libdragon resolution_t, e.g. RESOLUTION_320x240. */
void kiln_engine_init(resolution_t res);
void kiln_engine_close(void);

/** Fill a scene with sane defaults: 85 degree FOV, one white directional
 *  light, dim ambient, camera looking at the origin. */
void kiln_scene_init(KilnScene *s);

/** Recompute projection + view from the scene's camera fields. Call after
 *  moving the camera, before kiln_scene_begin. */
void kiln_scene_update(KilnScene *s);

/** Begin the 3D pass: attach the viewport, clear colour + depth, upload
 *  lights, and set the default 3D draw flags. */
void kiln_scene_begin(KilnScene *s);

/** Turn on this scene's fog and set its colour and range.
 *
 *  Fog is the cheapest thing on this hardware that makes distance read: it
 *  is already in the RSP vertex pipeline and in the blender, so a scene
 *  that fogs at all pays almost nothing for it. Matching `color` to
 *  whatever the horizon is (a skydome's lowest band, or the clear colour)
 *  is what makes far geometry end in haze rather than at a visible edge.
 *
 *  `near_`/`far_` are distances from the camera, in world units, and are
 *  independent of the projection's near/far — fog can, and usually should,
 *  finish before the far plane. */
void kiln_scene_set_fog(KilnScene *s, color_t color, float near_, float far_);

/** Turn fog off again. Scenes start with it off. */
void kiln_scene_disable_fog(KilnScene *s);

/** Project a world position to screen pixels through the scene's camera.
 *
 *  Writes the pixel position through sx/sy (either may be NULL) and returns 1
 *  when the point is in FRONT of the camera, 0 when it is behind. In the
 *  behind case sx/sy are still written, pushed out along the point's
 *  off-axis direction — a caller drawing an off-screen indicator wants a
 *  direction to clamp to, and a caller that wants to hide the marker
 *  instead just checks the return value. Neither case clamps to the
 *  viewport; that is a policy decision (clamp to edge vs. cull) the caller
 *  owns.
 *
 *  Reads only the scene's camera fields, so it does not need
 *  kiln_scene_update to have run this frame — but the result then describes
 *  where the point WILL be drawn, not where the last frame put it.
 *
 *  Costs no libm: fmath has no tanf, so the FOV term is computed as
 *  fm_sinf/fm_cosf, both of which inline. */
int kiln_scene_project(const KilnScene *s, fm_vec3_t world,
                      int screen_w, int screen_h, int *sx, int *sy);

/** View-space depth of a world point: its distance along the camera's forward
 *  axis. Positive in front of the camera, negative behind, zero on the camera
 *  plane. Independent of FOV and of the viewport, so it takes neither.
 *
 *  Exists so a caller that needs to CLIP against the camera plane — rather
 *  than merely know which side of it a point is on — can do so without
 *  re-deriving the camera basis. kiln_scene_project answers the yes/no question
 *  via its return value, which is enough for a point (kiln_target's reticle
 *  clamps to a screen edge) and not enough for a segment: joining an in-front
 *  endpoint to a behind-camera one requires the two depths to interpolate the
 *  crossing. kiln_debugdraw is the caller that needs this; putting it here
 *  rather than there keeps one derivation of `fwd` in the codebase, in the
 *  module that owns the camera fields. */
float kiln_scene_depth(const KilnScene *s, fm_vec3_t world);

/** Does this DFS path exist? Returns 1 if it does, 0 otherwise.
 *
 *  Needed because most libdragon/Tiny3D loaders ASSERT on a missing file
 *  rather than returning an error: wav64_open, xm64_open, sprite_load and
 *  t3d_model_load all end up in asset.c's must_open, which kills the ROM
 *  with "File not found" and drops it into the Inspector. A game that
 *  wants to degrade gracefully around an optional asset — art that has not
 *  been authored yet, a sound pack that ships later — cannot do it by
 *  checking a return value, because there is no return.
 *
 *  So probe first. This is the one place that knows the trick, so call
 *  sites read as an ordinary existence check instead of each reinventing
 *  a stdio dance. */
int kiln_dfs_exists(const char *dfs_path);

/** Frame bracket. kiln_frame_begin attaches the framebuffer and Z-buffer;
 *  kiln_frame_end presents it. */
void kiln_frame_begin(void);
void kiln_frame_end(void);

/** Transform helpers. kiln_transform_init allocates the uncached matrix. */
void kiln_transform_init(KilnTransform *t);
void kiln_transform_free(KilnTransform *t);

/** Rebuild the fixed-point matrix from pos/scale/rot and push it. Pair with
 *  kiln_transform_pop after drawing. */
void kiln_transform_push(KilnTransform *t);
void kiln_transform_pop(void);

#ifdef __cplusplus
}
#endif

#endif /* KILN_ENGINE_H */
