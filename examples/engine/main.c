// SPDX-License-Identifier: MIT
//
// The Kiln engine's two-layer model, demonstrated:
//
//   * a 3D pass  — a lit, fogged, depth-tested scene via Tiny3D on the RSP: a
//                  spinning cube, two satellites crossing in front of and
//                  behind it, and a checker floor fading into the fog
//   * a 2D pass  — a HUD of panels, bars and text via rdpq, composited on top
//
// The point of the example is the *seam* between them. Everything between
// kiln_scene_begin() and kiln_gui_begin() is perspective-projected and
// depth-tested; everything after kiln_gui_begin() is flat screen-space with
// depth off. One state transition per frame, in one obvious place.
//
// The geometry is kiln_prim: 24-vertex boxes with a true normal per face, so a
// lit cube reads as a solid rather than the soft blob eight shared corners
// make. Real projects use .t3dm models via gltf_to_t3d; this needs no assets.
//
//   stick  orbit the camera (left/right) and raise it (up/down)

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_prim.h>

#define SCREEN_W 320
#define SCREEN_H 240

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    kiln_input_init();

    KilnScene scene;
    kiln_scene_init(&scene);
    kiln_prim_stage(&scene, RGBA32(0x12, 0x14, 0x26, 0xFF), 110.0f, 300.0f);
    scene.fov_deg = 60.0f;
    scene.near_z = 8.0f;
    scene.far_z = 300.0f;

    /* The cube keeps its 14-unit half-extent: flake.nix sizes a model builder
     * against this demo's frustum. */
    KilnPrim cube, satellite, floor_prim, shadow;
    kiln_prim_box(&cube, (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 14, 14, 14 }},
                  kiln_prim_rgba(0x00, 0xF5, 0xD4), kiln_prim_rgba(0x8B, 0x5C, 0xF6), kiln_prim_rgba(0xFF, 0x4C, 0x6A));
    kiln_prim_box(&satellite, (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 5, 5, 5 }},
                  kiln_prim_rgba(0xFF, 0xE0, 0x60), kiln_prim_rgba(0xFF, 0x98, 0x30), kiln_prim_rgba(0x80, 0x40, 0x10));
    kiln_prim_floor(&floor_prim, 150.0f, 15, kiln_prim_rgba(0x2C, 0x32, 0x4C), kiln_prim_rgba(0x22, 0x28, 0x3E));
    kiln_prim_floor(&shadow, 16.0f, 1, kiln_prim_rgba(0x10, 0x12, 0x1E), kiln_prim_rgba(0x10, 0x12, 0x1E));

    /* Colour per vertex, not per face. kiln_prim gives each face a true normal
     * and one colour; the cube's corners are repainted here, once, with the
     * eight colours this demo has always had, so the faces Gouraud-blend
     * between them under correct lighting. The floor gets the same treatment
     * as a vignette. Written at build time only — rewriting a vertex buffer
     * between draws races the RSP's asynchronous read on console. */
    static const uint32_t CORNER[8] = {
        0xFF4C6AFF, 0x4CFF82FF, 0xFFD94CFF, 0x00F5D4FF,
        0x8B5CF6FF, 0xFF4C6AFF, 0x4CFF82FF, 0xFFD94CFF,
    };
    for (int vi = 0; vi < cube.vert_count; vi++) {
        T3DVertPacked *e = &cube.verts[vi / 2];
        const int16_t *p = (vi & 1) ? e->posB : e->posA;
        const uint32_t c = CORNER[(p[0] > 0) | ((p[1] > 0) << 1) | ((p[2] > 0) << 2)];
        if (vi & 1) e->rgbaB = c; else e->rgbaA = c;
    }
    for (int vi = 0; vi < floor_prim.vert_count; vi++) {
        T3DVertPacked *e = &floor_prim.verts[vi / 2];
        const int16_t *p = (vi & 1) ? e->posB : e->posA;
        const uint32_t base = (vi & 1) ? e->rgbaB : e->rgbaA;
        const float d = ((float)p[0] * p[0] + (float)p[2] * p[2]) / (150.0f * 150.0f * 2.0f);
        const uint32_t c = kiln_prim_shade(base, 1.6f - 1.1f * d);
        if (vi & 1) e->rgbaB = c; else e->rgbaA = c;
    }
    data_cache_hit_writeback(cube.verts, sizeof(T3DVertPacked) * 12);
    data_cache_hit_writeback(floor_prim.verts, sizeof(T3DVertPacked) * (size_t)floor_prim.quad_count * 2);

    KilnTransform cube_xf, sat_xf[2], floor_xf, shadow_xf;
    kiln_transform_init(&cube_xf);
    kiln_transform_init(&sat_xf[0]);
    kiln_transform_init(&sat_xf[1]);
    kiln_transform_init(&floor_xf);
    kiln_transform_init(&shadow_xf);
    cube_xf.pos = (fm_vec3_t){{ 0, 26, 0 }};
    cube_xf.rot_axis = (fm_vec3_t){{ 0.3f, 1.0f, 0.15f }};
    fm_vec3_norm(&cube_xf.rot_axis, &cube_xf.rot_axis);
    shadow_xf.pos = (fm_vec3_t){{ 0, 0.4f, 0 }};

    float spin = 0.0f, orbit = 0.0f, height = 46.0f;
    uint32_t frames = 0;
    float fps = 0.0f;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        // ── update ──────────────────────────────────────────────────
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);
        const float dt = 1.0f / 60.0f;

        spin += 0.02f;
        /* The stick orbits the camera round the cube; left alone it drifts. */
        orbit += (in->stick_x * 1.6f + 0.15f) * dt;
        height += in->stick_y * 60.0f * dt;
        if (height < 12.0f) height = 12.0f;
        if (height > 110.0f) height = 110.0f;
        scene.cam_pos = (fm_vec3_t){{ fm_sinf(orbit) * 96.0f, height, -fm_cosf(orbit) * 96.0f }};
        scene.cam_target = (fm_vec3_t){{ 0, 20, 0 }};
        kiln_scene_update(&scene);

        cube_xf.rot_angle = spin;
        for (int i = 0; i < 2; i++) {
            const float a = spin * (i ? -1.4f : 1.1f) + i * 3.14159f;
            sat_xf[i].pos = (fm_vec3_t){{ fm_cosf(a) * 34.0f, 26.0f + 10.0f * fm_sinf(a * 2.0f), fm_sinf(a) * 34.0f }};
            sat_xf[i].rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
            sat_xf[i].rot_angle = -spin * 3.0f;
        }

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        // ── 3D pass ─────────────────────────────────────────────────
        kiln_frame_begin();
        kiln_scene_begin(&scene);

        kiln_transform_push(&floor_xf);  kiln_prim_draw(&floor_prim); kiln_transform_pop();
        kiln_transform_push(&shadow_xf); kiln_prim_draw(&shadow);     kiln_transform_pop();
        kiln_transform_push(&cube_xf);   kiln_prim_draw(&cube);       kiln_transform_pop();
        for (int i = 0; i < 2; i++) {
            kiln_transform_push(&sat_xf[i]);
            kiln_prim_draw(&satellite);
            kiln_transform_pop();
        }

        // ── 2D pass ─────────────────────────────────────────────────
        // Everything below is screen-space with depth off. It is drawn after
        // the cube yet always appears in front of it — that is the depth-
        // disable in kiln_gui_begin() doing its job, not draw order luck.
        kiln_gui_begin();

        kiln_gui_panel(8, 8, 150, 46, RGBA32(10, 10, 24, 255), RGBA32(0, 245, 212, 255));
        kiln_gui_text(14, 22, RGBA32(0, 245, 212, 255), "KILN ENGINE");
        kiln_gui_text(14, 34, RGBA32(232, 232, 240, 255), "3D: tiny3d/RSP");
        kiln_gui_text(14, 46, RGBA32(232, 232, 240, 255), "2D: rdpq overlay");

        kiln_gui_panel(8, SCREEN_H - 40, SCREEN_W - 16, 32, RGBA32(10, 10, 24, 255), RGBA32(139, 92, 246, 255));
        kiln_gui_text(14, SCREEN_H - 26, RGBA32(232, 232, 240, 255), "fps %5.1f   spin %5.2f", fps, spin);
        kiln_gui_text(14, SCREEN_H - 14, RGBA32(144, 152, 176, 255), "stick: orbit + raise");

        // A meter, to show a widget that is not just text.
        kiln_gui_bar(150, SCREEN_H - 22, 150, 8,
                     (spin - (float)(int)(spin / 6.28f) * 6.28f) / 6.28f,
                     RGBA32(0, 245, 212, 255), RGBA32(42, 42, 62, 255));

        kiln_gui_end();
        kiln_frame_end();
    }
}
