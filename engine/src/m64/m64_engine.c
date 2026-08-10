/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_engine.c — 3D pass implementation. See m64_engine.h for the model.
 */

#include "m64_engine.h"
#include "m64_gui.h"
#include "m64_panic.h"

#include <malloc.h>
#include <stdio.h>

void m64_engine_init(resolution_t res)
{
    /* Three buffers, not two. The 3D pass hands work to the RSP and RDP and
     * then wants to keep the CPU busy; with only two buffers display_get()
     * blocks on the RDP far more often. Tiny3D's own examples use 3. */
    display_init(res, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE);
    rdpq_init();
    t3d_init((T3DInitParams){});
    m64_gui_init();

    /* Install the panic handler after GUI init so the module is fully wired,
     * even though the panic path uses the text console rather than the GUI.
     * This is automatic for every ROM that calls m64_engine_init(). */
    m64_panic_install();
}

int m64_dfs_exists(const char *dfs_path)
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

void m64_engine_close(void)
{
    m64_gui_close();
    t3d_destroy();
    rdpq_close();
    display_close();
}

void m64_scene_init(M64Scene *s)
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
    for (int i = 0; i < M64_SCENE_MAX_LIGHTS - 1; i++) {
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

void m64_scene_set_fog(M64Scene *s, color_t color, float near_, float far_)
{
    s->fog_enabled = 1;
    s->fog_color = color;
    s->fog_near = near_;
    s->fog_far = far_;
}

void m64_scene_disable_fog(M64Scene *s) { s->fog_enabled = 0; }

void m64_scene_update(M64Scene *s)
{
    t3d_viewport_set_projection(&s->viewport, T3D_DEG_TO_RAD(s->fov_deg),
                                s->near_z, s->far_z);
    t3d_viewport_look_at(&s->viewport, &s->cam_pos, &s->cam_target, &s->cam_up);
}

void m64_frame_begin(void)
{
    rdpq_attach(display_get(), display_get_zbuf());
    t3d_frame_start();
}

void m64_frame_end(void)
{
    rdpq_detach_show();
}

void m64_scene_begin(M64Scene *s)
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
    if (n > M64_SCENE_MAX_LIGHTS) n = M64_SCENE_MAX_LIGHTS;

    if (n > 0) t3d_light_set_directional(0, s->light_color, &s->light_dir);
    for (int i = 1; i < n; i++) {
        t3d_light_set_directional(i, s->lights[i - 1].color,
                                  &s->lights[i - 1].dir);
    }
    t3d_light_set_count(n);

    /* Fog is scene-wide state, so it is set once here rather than by
     * whichever model happens to draw first. */
    if (s->fog_enabled) {
        rdpq_set_fog_color(s->fog_color);
        t3d_fog_set_range(s->fog_near, s->fog_far);
        t3d_fog_set_enabled(true);
    } else {
        t3d_fog_set_enabled(false);
    }

    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH);
}

int m64_scene_project(const M64Scene *s, fm_vec3_t world,
                      int screen_w, int screen_h, int *sx, int *sy)
{
    /* View basis from the camera fields. Recomputed per call rather than
     * cached on the scene: a caller projecting a handful of points per
     * frame pays three normalises, and a cache would need invalidating on
     * every camera field write, which is exactly the kind of hidden
     * coupling this engine's explicit m64_scene_update bracket avoids. */
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

void m64_transform_init(M64Transform *t)
{
    t->pos = (fm_vec3_t){ { 0, 0, 0 } };
    t->scale = (fm_vec3_t){ { 1, 1, 1 } };
    t->rot_axis = (fm_vec3_t){ { 0, 1, 0 } };
    t->rot_angle = 0.0f;

    /* Uncached: the RSP DMAs this directly, so a cached write could still be
     * sitting in the CPU's write-back cache when the RSP reads the line. */
    t->mtx = malloc_uncached(sizeof(T3DMat4FP));
    assertf(t->mtx, "m64: out of memory allocating a transform matrix");
}

void m64_transform_free(M64Transform *t)
{
    if (t->mtx) {
        free_uncached(t->mtx);
        t->mtx = NULL;
    }
}

void m64_transform_push(M64Transform *t)
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

void m64_transform_pop(void)
{
    t3d_matrix_pop(1);
}
