/* SPDX-License-Identifier: MIT
 *
 * kiln_debugdraw.h — see the invisible things.
 *
 * Lines, boxes, axes and labels in WORLD space, drawn in the 2D pass. The
 * engine had no way to visualise any of its own spatial state: not a clip
 * brush, not an actor's bounds, not a trigger volume, not a camera path, not a
 * trace normal, not a tile boundary. Everything an engine knows about where
 * things are was reachable only as numbers printed by a HUD.
 *
 * That is a bad trade for 3D work, and a downstream game paid it repeatedly:
 * four real defects that each cost real time, and every one of them spatial
 * — a camera inside a landmark's own footprint, a shot opening dozens of
 * units behind a wall, a model quantised into a slab. That game's first
 * answer was to print eye/target/near/far as text and reason about it.
 * Drawing it is strictly better, and it is cheap.
 *
 * ── Screen space, on purpose ────────────────────────────────────────────
 * Every primitive here projects its world points through kiln_scene_project()
 * and draws the result with kiln_gui — it does NOT submit 3D geometry to
 * Tiny3D. Three reasons, in order of weight:
 *
 *   1. It cannot perturb what it is describing. A debug pass that pushed
 *      matrices, changed the combiner, or added a display list would change
 *      the frame it is meant to be reporting on. That failure mode — the
 *      instrument altering the reading — is worth more than the fidelity it
 *      would buy.
 *   2. Depth is already off in the 2D pass, so a brush behind a wall is still
 *      visible. For a debug overlay that is the correct behaviour and
 *      occlusion would be a bug: the whole point is to see the thing you
 *      cannot see.
 *   3. kiln_scene_project already exists and is already the engine's answer to
 *      "where does this world point land on screen" (kiln_target's reticle uses
 *      it). A second projection path would be a second thing to get wrong.
 *
 * The cost is that lines are flat 1-pixel-ish screen lines with no perspective
 * thickness and no depth sorting. For a wireframe box that is invisible; for
 * anything that wants to look good, this is the wrong module.
 *
 * ── It is not free ──────────────────────────────────────────────────────
 * Each line is two rdpq triangles and each label is a text draw, and both go
 * through the 2D pass's blender. Measured in real use: about 37 lines plus a
 * dozen labels takes a 60 fps scene to 54. That is the right trade for a
 * debug overlay and the wrong one for anything shipped — which is why the
 * layers are opt-in and default to off. If a layer ever needs to be on while
 * judging frame time, note that the number it is reporting is not the number
 * without it.
 *
 * ── Near-plane clipping is not optional ─────────────────────────────────
 * kiln_scene_project reports in_front = 0 for a point behind the camera and
 * pushes it far out along its own off-axis direction, deliberately leaving the
 * clamp-or-cull policy to the caller (see its comment). That is right for
 * kiln_target's reticle, which clamps to the nearer screen edge. It is wrong
 * for a line: a segment with one endpoint behind the camera would draw as a
 * wild streak across the frame, and a wireframe box straddling the camera
 * would become a starburst — noise that looks like a bug in the geometry
 * rather than in the overlay. So every segment is clipped against the camera
 * plane in view space BEFORE projecting. See kiln_dd_line's implementation.
 *
 * ── Costs nothing when unused ───────────────────────────────────────────
 * The symbols are ALWAYS in libkiln.a and this header is NOT gated on
 * KILN_DEBUG — the same contract kiln_console.h documents, and for a
 * structural reason rather than a stylistic one: nix/engine.nix builds the
 * library exactly once, without KILN_DEBUG, so a module that compiled itself
 * away would leave a ROM built with `debugConsole = true` linking against
 * symbols that do not exist. (This module was written the other way first;
 * that is the bug it produced.)
 *
 * A ROM that never calls these pays nothing anyway, because libdragon's link
 * uses --gc-sections. A ROM that wants the calls themselves to vanish gates
 * them: one `#ifdef KILN_DEBUG` around the game's own debug module rather
 * than one per call site.
 *
 * ── Usage ───────────────────────────────────────────────────────────────
 *   kiln_frame_begin();
 *     kiln_scene_begin(&scene);
 *       ... draw the world ...
 *     kiln_gui_begin();
 *       ... HUD ...
 *       kiln_dd_begin(&scene, w, h);     <- after the scene is final
 *         kiln_dd_aabb(brush.mins, brush.maxs, RGBA32(255,0,0,160));
 *         kiln_dd_axes(origin, 64.0f);
 *       kiln_dd_end();
 *     kiln_gui_end();
 *   kiln_frame_end();
 *
 * `scene` must be the scene the frame was actually built from — after the
 * camera has been applied and after anything else (a veil, a cutscene) has
 * written to it. Handing it a scene that was merely initialised draws a
 * perfectly plausible overlay of the wrong camera, which is worse than no
 * overlay at all.
 */
#ifndef KILN_DEBUGDRAW_H
#define KILN_DEBUGDRAW_H

#include <stdint.h>
#include <libdragon.h>
#include <t3d/t3dmath.h>

#include "kiln_engine.h"

#ifdef __cplusplus
extern "C" {
#endif


/** Begin a debug-draw block. Records the scene and viewport every subsequent
 *  call projects through. Call inside the 2D pass (between kiln_gui_begin and
 *  kiln_gui_end); `scene` is borrowed and must outlive the block. */
void kiln_dd_begin(const KilnScene *scene, int screen_w, int screen_h);

/** End the block. Restores nothing — every primitive here leaves the RDP in
 *  the state kiln_gui_begin established — but marks the scene pointer stale so
 *  a stray call outside the block is a no-op rather than a read of a dangling
 *  pointer. */
void kiln_dd_end(void);

/** World-space segment. Clipped against the camera plane; a segment entirely
 *  behind the camera draws nothing. */
void kiln_dd_line(fm_vec3_t a, fm_vec3_t b, color_t c);

/** Twelve-edge wireframe box. The workhorse: a clip brush, an actor's bounds,
 *  a trigger volume and a tile footprint are all this. */
void kiln_dd_aabb(fm_vec3_t mins, fm_vec3_t maxs, color_t c);

/** Wireframe box centred on `centre` with half-extents `half`. The form
 *  kiln_clip and kiln_actor state their volumes in, so a caller does not have to
 *  do the add/subtract at every site. */
void kiln_dd_box(fm_vec3_t centre, fm_vec3_t half, color_t c);

/** Three axis segments of `len` from `origin`, coloured by ENGINE axis:
 *  red = +X, green = +Y (up), blue = +Z. Deliberately the engine's axes and
 *  not Blender's — the whole class of bug this helps with is a +Z-up authoring
 *  convention meeting a +Y-up runtime, and a gizmo drawn in the authoring
 *  convention would confirm the mistake instead of exposing it. */
void kiln_dd_axes(fm_vec3_t origin, float len);

/** A small screen-space cross at a world point. `px` is its half-size in
 *  pixels, so a marker stays legible at any distance — which is what makes it
 *  usable for "where is the MRI" as opposed to "how big is the MRI". */
void kiln_dd_point(fm_vec3_t p, int px, color_t c);

/** The viewing volume of a camera that is NOT the one you are looking through.
 *
 *  Eight corners and twelve edges, plus a diagonal across each of the near and
 *  far rectangles so the two planes read as planes rather than as rings. `up`
 *  is assumed to be world +Y, the same assumption kiln_scene_update makes.
 *
 *  This is the primitive for the failure mode nothing else exposes: a frustum
 *  is two numbers in a struct, and both of the ways it goes wrong — a far plane
 *  in front of the thing being framed, a near plane past it — render as the
 *  subject simply not being there. That is indistinguishable from a model that
 *  failed to load, and telling those two apart by moving the camera and
 *  rebuilding is where the time goes. Drawn from a second camera, it is
 *  obvious.
 *
 *  Only meaningful from a DETACHED viewpoint. Drawn from inside itself, a
 *  frustum is a full-screen X. */
void kiln_dd_frustum(fm_vec3_t eye, fm_vec3_t look, float fov_deg, float aspect,
                    float near_z, float far_z, color_t c);

/** Polyline through `n` world points. `n < 2` draws nothing.
 *
 *  This is the one that pays for the module on a cutscene: a keyframed camera
 *  path is a list of eye positions, and Catmull-Rom overshoot between unevenly
 *  spaced keys is instantly obvious as a drawn curve and effectively invisible
 *  as a table of numbers. */
void kiln_dd_path(const fm_vec3_t *pts, int n, color_t c);

/** Text anchored to a world point, printf-style. Nothing is drawn when the
 *  point is behind the camera — a label is not worth clamping to an edge,
 *  where it would name something the viewer cannot see. */
void kiln_dd_text(fm_vec3_t p, color_t c, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/** How many primitives the current block has drawn, and how many it dropped
 *  as entirely behind the camera. Worth surfacing rather than keeping private:
 *  "the overlay is drawing nothing" and "the overlay is drawing and you are
 *  looking the wrong way" are the two states a blank debug pass could be in,
 *  and a HUD line reading `dd 0/48` tells them apart immediately. */
uint16_t kiln_dd_drawn(void);
uint16_t kiln_dd_clipped(void);


#ifdef __cplusplus
}
#endif

#endif /* KILN_DEBUGDRAW_H */
