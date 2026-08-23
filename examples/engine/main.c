// SPDX-License-Identifier: MIT
//
// The Kiln engine's two-layer model, demonstrated:
//
//   * a 3D pass  — a lit, depth-tested, spinning object via Tiny3D on the RSP
//   * a 2D pass  — a HUD of panels, bars and text via rdpq, composited on top
//
// The point of the example is the *seam* between them. Everything between
// kiln_scene_begin() and kiln_gui_begin() is perspective-projected and
// depth-tested; everything after kiln_gui_begin() is flat screen-space with
// depth off. One state transition per frame, in one obvious place.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>

#include <malloc.h>

#define SCREEN_W 320
#define SCREEN_H 240

// A cube built by hand. Tiny3D packs two vertices per T3DVertPacked struct,
// so 8 corners is 4 structs. Real projects use the .t3dm model format via
// gltf_to_t3d; this is here so the example needs no asset pipeline at all.
#define CUBE_VERTS 8

static T3DVertPacked *make_cube(void)
{
    // Uncached: the RSP DMAs vertex data straight out of RDRAM.
    T3DVertPacked *v = malloc_uncached(sizeof(T3DVertPacked) * (CUBE_VERTS / 2));

    const int16_t s = 14;
    // Normals are packed 5.6.5. For a flat-shaded cube the per-corner normal
    // is a reasonable approximation and keeps the vertex count at 8 rather
    // than the 24 a per-face normal would need.
    struct { int16_t x, y, z; uint32_t rgba; } c[CUBE_VERTS] = {
        { -s, -s, -s, 0xFF4C6AFF }, {  s, -s, -s, 0x4CFF82FF },
        {  s,  s, -s, 0xFFD94CFF }, { -s,  s, -s, 0x00F5D4FF },
        { -s, -s,  s, 0x8B5CF6FF }, {  s, -s,  s, 0xFF4C6AFF },
        {  s,  s,  s, 0x4CFF82FF }, { -s,  s,  s, 0xFFD94CFF },
    };

    for (int i = 0; i < CUBE_VERTS; i += 2) {
        fm_vec3_t na = {{ (float)c[i].x, (float)c[i].y, (float)c[i].z }};
        fm_vec3_t nb = {{ (float)c[i+1].x, (float)c[i+1].y, (float)c[i+1].z }};
        fm_vec3_norm(&na, &na);
        fm_vec3_norm(&nb, &nb);

        v[i / 2] = (T3DVertPacked){
            .posA = { c[i].x, c[i].y, c[i].z },
            .rgbaA = c[i].rgba,
            .normA = t3d_vert_pack_normal(&na),
            .posB = { c[i+1].x, c[i+1].y, c[i+1].z },
            .rgbaB = c[i+1].rgba,
            .normB = t3d_vert_pack_normal(&nb),
        };
    }
    return v;
}


int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();

    KilnScene scene;
    kiln_scene_init(&scene);

    KilnTransform cube;
    kiln_transform_init(&cube);
    cube.scale = (fm_vec3_t){{ 1.0f, 1.0f, 1.0f }};

    // Pull the camera back until the whole cube fits: at the default -32 with
    // an 85-degree FOV a 28-unit cube overflows the frustum and reads as a
    // flat wall rather than a solid.
    scene.cam_pos = (fm_vec3_t){{ 0, 14, -70 }};
    scene.far_z = 300.0f;

    T3DVertPacked *verts = make_cube();

    // The 12 triangles of the cube, as index triples into the 8 corners.
    static const uint8_t tris[12][3] = {
        {0,1,2},{2,3,0}, {4,6,5},{6,4,7},
        {0,4,5},{5,1,0}, {1,5,6},{6,2,1},
        {2,6,7},{7,3,2}, {3,7,4},{4,0,3},
    };

    float spin = 0.0f;
    uint32_t frames = 0;
    float fps = 0.0f;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        // ── update ──────────────────────────────────────────────────
        joypad_poll();
        joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);

        spin += 0.02f;
        // Stick nudges the camera so the depth relationship is visible.
        scene.cam_pos.v[0] = (float)in.stick_x * 0.25f;
        scene.cam_pos.v[1] = 14.0f + (float)in.stick_y * 0.25f;
        kiln_scene_update(&scene);

        cube.rot_angle = spin;
        cube.rot_axis = (fm_vec3_t){{ 0.3f, 1.0f, 0.15f }};
        fm_vec3_norm(&cube.rot_axis, &cube.rot_axis);

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        // ── 3D pass ─────────────────────────────────────────────────
        kiln_frame_begin();
        kiln_scene_begin(&scene);

        kiln_transform_push(&cube);
        t3d_vert_load(verts, 0, CUBE_VERTS);
        kiln_transform_pop();

        for (int i = 0; i < 12; i++) {
            t3d_tri_draw(tris[i][0], tris[i][1], tris[i][2]);
        }
        t3d_tri_sync();

        // ── 2D pass ─────────────────────────────────────────────────
        // Everything below is screen-space with depth off. Note it is drawn
        // after the cube yet always appears in front of it — that is the
        // depth-disable in kiln_gui_begin() doing its job, not draw order luck.
        kiln_gui_begin();

        kiln_gui_panel(8, 8, 150, 46,
                      RGBA32(10, 10, 24, 200), RGBA32(0, 245, 212, 255));
        kiln_gui_text(14, 22, RGBA32(0, 245, 212, 255), "KILN ENGINE");
        kiln_gui_text(14, 34, RGBA32(232, 232, 240, 255), "3D: tiny3d/RSP");
        kiln_gui_text(14, 46, RGBA32(232, 232, 240, 255), "2D: rdpq overlay");

        kiln_gui_panel(8, SCREEN_H - 40, SCREEN_W - 16, 32,
                      RGBA32(10, 10, 24, 200), RGBA32(139, 92, 246, 255));
        kiln_gui_text(14, SCREEN_H - 26, RGBA32(232, 232, 240, 255),
                     "fps %5.1f   spin %5.2f", fps, spin);

        // A meter, to show a widget that is not just text.
        kiln_gui_bar(150, SCREEN_H - 22, 150, 8,
                    (spin - (float)(int)(spin / 6.28f) * 6.28f) / 6.28f,
                    RGBA32(0, 245, 212, 255), RGBA32(42, 42, 62, 255));

        kiln_gui_end();
        kiln_frame_end();
    }
}
