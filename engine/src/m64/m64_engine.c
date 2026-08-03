/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_engine.c — 3D pass implementation. See m64_engine.h for the model.
 */

#include "m64_engine.h"
#include "m64_gui.h"

#include <malloc.h>

void m64_engine_init(resolution_t res)
{
    /* Three buffers, not two. The 3D pass hands work to the RSP and RDP and
     * then wants to keep the CPU busy; with only two buffers display_get()
     * blocks on the RDP far more often. Tiny3D's own examples use 3. */
    display_init(res, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE);
    rdpq_init();
    t3d_init((T3DInitParams){});
    m64_gui_init();
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

    s->clear_color = RGBA32(10, 10, 24, 0xFF);
}

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
    t3d_light_set_directional(0, s->light_color, &s->light_dir);
    t3d_light_set_count(s->light_count);

    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH);
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
