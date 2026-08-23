/* SPDX-License-Identifier: MIT
 *
 * kiln_engine.c — 3D pass implementation. See kiln_engine.h for the model.
 */

#include "kiln_engine.h"
#include "kiln_gui.h"
#include "kiln_panic.h"

#include <malloc.h>
#include <stdio.h>

void kiln_engine_init(resolution_t res)
{
    /* Three buffers, not two. The 3D pass hands work to the RSP and RDP and
     * then wants to keep the CPU busy; with only two buffers display_get()
     * blocks on the RDP far more often. Tiny3D's own examples use 3. */
    display_init(res, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE);
    rdpq_init();
    t3d_init((T3DInitParams){});
    kiln_gui_init();

    /* Install the panic handler after GUI init so the module is fully wired,
     * even though the panic path uses the text console rather than the GUI.
     * This is automatic for every ROM that calls kiln_engine_init(). */
    kiln_panic_install();
}

int kiln_dfs_exists(const char *dfs_path)
{
    if (!dfs_path) return 0;
    /* stdio rather than dfs_rom_addr: dfs_open wants a path without the
     * "rom:/" prefix every caller here already has, and fopen goes through
     * the same filesystem hook the asserting loaders use — so this answers
     * the question they are about to ask, in their terms. */
    FILE *f = fopen(dfs_path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

void kiln_engine_close(void)
{
    kiln_gui_close();
    t3d_destroy();
    rdpq_close();
    display_close();
}

void kiln_scene_init(KilnScene *s)
{
    s->viewport = t3d_viewport_create();

    s->cam_pos = (fm_vec3_t){ { 0, 8, -32 } };
    s->cam_target = (fm_vec3_t){ { 0, 0, 0 } };
    s->cam_up = (fm_vec3_t){ { 0, 1, 0 } };

    s->fov_deg = 85.0f;
    s->near_z = 10.0f;
    s->far_z = 200.0f;

    /* Ambient is deliberately non-zero: with a single directional light,
     * faces pointing away from it would otherwise be pure black and the
     * silhouette would vanish against the clear colour. */
    s->ambient[0] = s->ambient[1] = s->ambient[2] = 45;
    s->ambient[3] = 0xFF;

    s->light_color[0] = s->light_color[1] = s->light_color[2] = 0xFF;
    s->light_color[3] = 0xFF;

    s->light_dir = (fm_vec3_t){ { 1.0f, 1.0f, 1.0f } };
    fm_vec3_norm(&s->light_dir, &s->light_dir);
    s->light_count = 1;

    /* Lights 1.. start black and pointing up. A scene that raises
     * light_count without filling them gets no light rather than the
     * uninitialised RSP slots the previous single-light upload would
     * have handed it. */
    for (int i = 0; i < KILN_SCENE_MAX_LIGHTS - 1; i++) {
        s->lights[i].color[0] = s->lights[i].color[1] = 0;
        s->lights[i].color[2] = 0;
        s->lights[i].color[3] = 0xFF;
        s->lights[i].dir = (fm_vec3_t){ { 0.0f, 1.0f, 0.0f } };
    }

    s->fog_enabled = 0;
    s->fog_color = RGBA32(0, 0, 0, 0xFF);
    s->fog_near = 0.0f;
    s->fog_far = 0.0f;

    s->clear_color = RGBA32(10, 10, 24, 0xFF);
}

void kiln_scene_set_fog(KilnScene *s, color_t color, float near_, float far_)
{
    s->fog_enabled = 1;
    s->fog_color = color;
    s->fog_near = near_;
    s->fog_far = far_;
}

void kiln_scene_disable_fog(KilnScene *s) { s->fog_enabled = 0; }

void kiln_scene_update(KilnScene *s)
{
    t3d_viewport_set_projection(&s->viewport, T3D_DEG_TO_RAD(s->fov_deg),
                                s->near_z, s->far_z);
    t3d_viewport_look_at(&s->viewport, &s->cam_pos, &s->cam_target, &s->cam_up);
}

void kiln_frame_begin(void)
{
    rdpq_attach(display_get(), display_get_zbuf());
    t3d_frame_start();
}

void kiln_frame_end(void)
{
    rdpq_detach_show();
}

void kiln_scene_begin(KilnScene *s)
{
    t3d_viewport_attach(&s->viewport);

    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    t3d_screen_clear_color(s->clear_color);
    t3d_screen_clear_depth();

    t3d_light_set_ambient(s->ambient);

    /* Upload every light the scene claims, not just light 0. Uploading one
     * while telling the RSP there were `light_count` left lights 1.. as
     * whatever happened to be in those slots — which looked like a scene
     * lit from a random direction that changed when an unrelated ROM ran
     * first. Clamped because t3d's slots are finite and a caller with a
     * stale count should get a dim scene, not a corrupt one. */
    int n = s->light_count;
    if (n < 0) n = 0;
    if (n > KILN_SCENE_MAX_LIGHTS) n = KILN_SCENE_MAX_LIGHTS;

    if (n > 0) t3d_light_set_directional(0, s->light_color, &s->light_dir);
    for (int i = 1; i < n; i++) {
        t3d_light_set_directional(i, s->lights[i - 1].color,
                                  &s->lights[i - 1].dir);
    }
    t3d_light_set_count(n);

    /* ── Fog needs BOTH halves ──────────────────────────────────────────
     * N64 fog is cooperative: the RSP writes a per-vertex depth factor into
     * SHADE ALPHA, and the RDP blender lerps each pixel toward the fog
     * colour using it. This used to do only the RSP half —
     * t3d_fog_set_range + t3d_fog_set_enabled — plus rdpq_set_fog_color,
     * and never called rdpq_mode_fog.
     *
     * Setting the fog COLOUR is not enabling the fog STAGE. libdragon says
     * so directly (rdpq_mode.h): "rdpq assumes that this has already been
     * done when rdpq_mode_fog is called ... To enable fog, pass
     * RDPQ_FOG_STANDARD to this function, and call rdpq_set_fog_color."
     * So the RSP computed a fog factor every frame and the blender threw it
     * away: there was no visible fog in any ROM built on this engine, and
     * every fog range anyone tuned was inert.
     *
     * It has to be re-armed HERE, every frame, because t3d_frame_start()
     * ends with an explicit rdpq_mode_fog(0) (t3d.c) — and kiln_frame_begin
     * calls that immediately before this. rdpq_set_mode_standard() in the
     * 2D pass clears it too, for the same reason and with the same fix.
     *
     * rdpq_mode_combiner above does NOT clear fog, and rdpq auto-adjusts
     * RDPQ_COMBINER_SHADE / TEX_SHADE so they stop using shade alpha as a
     * modulation factor once fog is on — both of the combiners this engine
     * uses are in that auto-handled set.
     *
     * Note the polarity when tuning: RDPQ_FOG_STANDARD is
     * IN_RGB*SHADE_ALPHA + FOG_RGB*(1-SHADE_ALPHA), so shade alpha 0 is
     * FULLY fogged and 255 is clear. */
    rdpq_mode_fog(s->fog_enabled ? RDPQ_FOG_STANDARD : 0);
    if (s->fog_enabled) {
        rdpq_set_fog_color(s->fog_color);
        t3d_fog_set_range(s->fog_near, s->fog_far);
        t3d_fog_set_enabled(true);
    } else {
        t3d_fog_set_enabled(false);
    }

    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH);
}

int kiln_scene_project(const KilnScene *s, fm_vec3_t world,
                      int screen_w, int screen_h, int *sx, int *sy)
{
    /* View basis from the camera fields. Recomputed per call rather than
     * cached on the scene: a caller projecting a handful of points per
     * frame pays three normalises, and a cache would need invalidating on
     * every camera field write, which is exactly the kind of hidden
     * coupling this engine's explicit kiln_scene_update bracket avoids. */
    fm_vec3_t fwd = {{ s->cam_target.v[0] - s->cam_pos.v[0],
                       s->cam_target.v[1] - s->cam_pos.v[1],
                       s->cam_target.v[2] - s->cam_pos.v[2] }};
    fm_vec3_norm(&fwd, &fwd);
    fm_vec3_t up = s->cam_up;
    fm_vec3_t right;
    fm_vec3_cross(&right, &fwd, &up);
    fm_vec3_norm(&right, &right);
    fm_vec3_t real_up;
    fm_vec3_cross(&real_up, &right, &fwd);

    fm_vec3_t d = {{ world.v[0] - s->cam_pos.v[0],
                     world.v[1] - s->cam_pos.v[1],
                     world.v[2] - s->cam_pos.v[2] }};
    float vz = d.v[0] * fwd.v[0]     + d.v[1] * fwd.v[1]     + d.v[2] * fwd.v[2];
    float vx = d.v[0] * right.v[0]   + d.v[1] * right.v[1]   + d.v[2] * right.v[2];
    float vy = d.v[0] * real_up.v[0] + d.v[1] * real_up.v[1] + d.v[2] * real_up.v[2];

    float nx, ny;
    int in_front;
    if (vz <= 0.001f) {
        /* Behind (or on) the camera plane: there is no valid projection, so
         * push the point far out along its own off-axis direction and let
         * the caller decide whether to clamp or cull. */
        float inv = 1.0f / (vz < 0 ? -1e-3f : 1e-3f);
        nx = vx * inv;
        ny = vy * inv;
        in_front = 0;
    } else {
        float aspect = (float)screen_w / (float)screen_h;
        float half = T3D_DEG_TO_RAD(s->fov_deg) * 0.5f;
        /* fmath has no tanf (it would be a libm call); sinf/cosf inline, so
         * tan = sin/cos costs two multiplies and a divide, no libm. */
        float tan_half_y = fm_sinf(half) / fm_cosf(half);
        float tan_half_x = tan_half_y * aspect;
        nx = (vx / vz) / tan_half_x;
        ny = (vy / vz) / tan_half_y;
        in_front = 1;
    }

    if (sx) *sx = (int)((nx + 1.0f) * 0.5f * screen_w);
    if (sy) *sy = (int)((1.0f - ny) * 0.5f * screen_h);
    return in_front;
}

float kiln_scene_depth(const KilnScene *s, fm_vec3_t world)
{
    /* Same `fwd` as kiln_scene_project derives above, and kept adjacent to it
     * on purpose: these two are the only places the camera's forward axis is
     * computed, and a divergence between them would make a clipped segment
     * disagree with the projection of its own endpoints. */
    fm_vec3_t fwd = {{ s->cam_target.v[0] - s->cam_pos.v[0],
                       s->cam_target.v[1] - s->cam_pos.v[1],
                       s->cam_target.v[2] - s->cam_pos.v[2] }};
    fm_vec3_norm(&fwd, &fwd);

    fm_vec3_t d = {{ world.v[0] - s->cam_pos.v[0],
                     world.v[1] - s->cam_pos.v[1],
                     world.v[2] - s->cam_pos.v[2] }};
    return d.v[0] * fwd.v[0] + d.v[1] * fwd.v[1] + d.v[2] * fwd.v[2];
}

void kiln_transform_init(KilnTransform *t)
{
    t->pos = (fm_vec3_t){ { 0, 0, 0 } };
    t->scale = (fm_vec3_t){ { 1, 1, 1 } };
    t->rot_axis = (fm_vec3_t){ { 0, 1, 0 } };
    t->rot_angle = 0.0f;

    /* Uncached: the RSP DMAs this directly, so a cached write could still be
     * sitting in the CPU's write-back cache when the RSP reads the line. */
    t->mtx = malloc_uncached(sizeof(T3DMat4FP));
    assertf(t->mtx, "kiln: out of memory allocating a transform matrix");
}

void kiln_transform_free(KilnTransform *t)
{
    if (t->mtx) {
        free_uncached(t->mtx);
        t->mtx = NULL;
    }
}

void kiln_transform_push(KilnTransform *t)
{
    fm_mat4_t m;
    fm_mat4_identity(&m);
    fm_mat4_from_axis_angle(&m, &t->rot_axis, t->rot_angle);
    fm_mat4_scale(&m, &t->scale);

    /* Translation is applied after rotation and scale so `pos` means a world
     * position rather than "displacement along the rotated axes". */
    m.m[3][0] = t->pos.v[0];
    m.m[3][1] = t->pos.v[1];
    m.m[3][2] = t->pos.v[2];

    t3d_mat4_to_fixed(t->mtx, &m);
    t3d_matrix_push(t->mtx);
}

void kiln_transform_pop(void)
{
    t3d_matrix_pop(1);
}
