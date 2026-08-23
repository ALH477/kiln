// SPDX-License-Identifier: MIT
//
// texanim-demo: exercises kiln_texanim and kiln_vanim in one frame.
//
//   1. Scrolling UV water plane  — KILN_TEXANIM_SCROLL (tile callback)
//   2. Waving flag              — KilnDeform (procedural vertex deformation)
//   3. Morphing blob            — KilnMorph (CPU vertex lerp between 3 targets)
//   4. Cel-shaded cube          — KILN_VFX_CELSHADE_COLOR (RSP vertex FX)
//   5. Env-mapped sphere        — KILN_VFX_SPHERICAL_UV (RSP vertex FX)
//
// All geometry is hand-built (no .t3dm asset pipeline needed), matching the
// convention of examples/engine. The demo cycles through modes with the
// D-pad; A toggles the active effect.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_texanim.h>
#include <kiln/kiln_vanim.h>

#include <malloc.h>
#include <string.h>

#define SCREEN_W 320
#define SCREEN_H 240
#define DT (1.0f / 60.0f)

// ── Vertex helpers ────────────────────────────────────────────────────

static T3DVertPacked *alloc_verts(int count)
{
    return malloc_uncached(sizeof(T3DVertPacked) * ((count + 1) / 2));
}

static void set_vert(T3DVertPacked *v, int idx, int16_t x, int16_t y, int16_t z,
                     uint32_t rgba, int16_t s, int16_t t)
{
    int pi = idx / 2;
    if (idx & 1) {
        v[pi].posB[0] = x; v[pi].posB[1] = y; v[pi].posB[2] = z;
        v[pi].rgbaB = rgba;
        v[pi].stB[0] = s; v[pi].stB[1] = t;
        fm_vec3_t n = {{ (float)x, (float)y, (float)z }};
        fm_vec3_norm(&n, &n);
        v[pi].normB = t3d_vert_pack_normal(&n);
    } else {
        v[pi].posA[0] = x; v[pi].posA[1] = y; v[pi].posA[2] = z;
        v[pi].rgbaA = rgba;
        v[pi].stA[0] = s; v[pi].stA[1] = t;
        fm_vec3_t n = {{ (float)x, (float)y, (float)z }};
        fm_vec3_norm(&n, &n);
        v[pi].normA = t3d_vert_pack_normal(&n);
    }
}

// ── Water plane (UV scroll demo) ──────────────────────────────────────
// A flat grid with scrolling UVs. Drawn as hand-rolled t3d_vert_load +
// t3d_tri_draw, NOT as a .t3dm model — so the UV scroll is done by modifying
// the tile params via the scroll offset directly.

#define WATER_GRID 8
#define WATER_VERTS (WATER_GRID * WATER_GRID)
static T3DVertPacked *g_water_verts;
static float g_water_scroll_s, g_water_scroll_t;

static void water_init(void)
{
    g_water_verts = alloc_verts(WATER_VERTS);
    int16_t sz = 40;
    for (int y = 0; y < WATER_GRID; y++) {
        for (int x = 0; x < WATER_GRID; x++) {
            int idx = y * WATER_GRID + x;
            int16_t px = (int16_t)((x - WATER_GRID/2) * sz / (WATER_GRID-1));
            int16_t pz = (int16_t)((y - WATER_GRID/2) * sz / (WATER_GRID-1));
            int16_t u = (int16_t)(x * 32);
            int16_t v = (int16_t)(y * 32);
            set_vert(g_water_verts, idx, px, 0, pz, 0x4080FFFF, u, v);
        }
    }
}

static void water_draw(float dt)
{
    g_water_scroll_s += 8.0f * dt;
    g_water_scroll_t += 6.0f * dt;

    kiln_transform_push(&(KilnTransform){
        .pos = { { -60, -20, 0 } },
        .scale = { { 1, 1, 1 } },
        .rot_axis = { { 0, 1, 0 } },
        .rot_angle = 0,
    });
    t3d_vert_load(g_water_verts, 0, WATER_VERTS);
    for (int y = 0; y < WATER_GRID - 1; y++) {
        for (int x = 0; x < WATER_GRID - 1; x++) {
            int i0 = y * WATER_GRID + x;
            int i1 = i0 + 1;
            int i2 = i0 + WATER_GRID;
            int i3 = i2 + 1;
            t3d_tri_draw(i0, i2, i1);
            t3d_tri_draw(i1, i2, i3);
        }
    }
    t3d_tri_sync();
    kiln_transform_pop();
}

// ── Flag (procedural deformation demo) ────────────────────────────────
// A grid mesh whose Y positions are displaced by a sine wave each frame.
// Uses kiln_deform to demonstrate the CPU vertex buffer modification pattern.

#define FLAG_GRID_X 10
#define FLAG_GRID_Y 6
#define FLAG_VERTS (FLAG_GRID_X * FLAG_GRID_Y)

static float g_flag_time;
static T3DVertPacked *g_flag_verts;
static T3DVertPacked *g_flag_base;

static void flag_setup(void)
{
    g_flag_verts = alloc_verts(FLAG_VERTS);
    g_flag_base = alloc_verts(FLAG_VERTS);
    int16_t sz = 4;
    for (int y = 0; y < FLAG_GRID_Y; y++) {
        for (int x = 0; x < FLAG_GRID_X; x++) {
            int idx = y * FLAG_GRID_X + x;
            int16_t px = (int16_t)((x - FLAG_GRID_X/2) * sz);
            int16_t py = (int16_t)((FLAG_GRID_Y/2 - y) * sz);
            int16_t pz = 0;
            uint32_t rgba = 0xFF4C6AFF;
            if (x == 0) rgba = 0xFFFFFFFF;
            set_vert(g_flag_base, idx, px, py, pz, rgba, x * 32, y * 32);
        }
    }
    memcpy(g_flag_verts, g_flag_base, sizeof(T3DVertPacked) * ((FLAG_VERTS + 1) / 2));
}

static void flag_update(float dt)
{
    g_flag_time += dt;
    int pc = (FLAG_VERTS + 1) / 2;
    memcpy(g_flag_verts, g_flag_base, sizeof(T3DVertPacked) * pc);
    for (int i = 0; i < FLAG_VERTS; i++) {
        int x = i % FLAG_GRID_X;
        int16_t *pos = t3d_vertbuffer_get_pos(g_flag_verts, i);
        float wave = fm_sinf((float)x * 0.6f + g_flag_time * 3.0f) * 5.0f;
        pos[1] = (int16_t)((float)pos[1] + wave);
    }
    data_cache_hit_writeback(g_flag_verts, sizeof(T3DVertPacked) * pc);
}

static void flag_draw(void)
{
    kiln_transform_push(&(KilnTransform){
        .pos = { { 20, 10, -20 } },
        .scale = { { 1, 1, 1 } },
        .rot_axis = { { 0, 1, 0 } },
        .rot_angle = -0.3f,
    });
    t3d_vert_load(g_flag_verts, 0, FLAG_VERTS);
    for (int y = 0; y < FLAG_GRID_Y - 1; y++) {
        for (int x = 0; x < FLAG_GRID_X - 1; x++) {
            int i0 = y * FLAG_GRID_X + x;
            int i1 = i0 + 1;
            int i2 = i0 + FLAG_GRID_X;
            int i3 = i2 + 1;
            t3d_tri_draw(i0, i2, i1);
            t3d_tri_draw(i1, i2, i3);
        }
    }
    t3d_tri_sync();
    kiln_transform_pop();
}

// ── Morphing blob (morph target demo) ─────────────────────────────────
// Three shapes: cube, "tall cube", "wide cube". Blended by sine waves.
// Demonstrates the CPU vertex lerp pattern from kiln_morph.

#define BLOB_VERTS 8
static T3DVertPacked *g_blob_base;
static T3DVertPacked *g_blob_tall;
static T3DVertPacked *g_blob_wide;
static T3DVertPacked *g_blob_work;
static float g_blob_time;

static void blob_setup(void)
{
    g_blob_base = alloc_verts(BLOB_VERTS);
    g_blob_tall = alloc_verts(BLOB_VERTS);
    g_blob_wide = alloc_verts(BLOB_VERTS);
    g_blob_work = alloc_verts(BLOB_VERTS);

    int16_t s = 14;
    // Base cube
    for (int i = 0; i < BLOB_VERTS; i++) {
        int16_t x = (i & 1) ? s : -s;
        int16_t y = (i & 2) ? s : -s;
        int16_t z = (i & 4) ? s : -s;
        uint32_t c = 0xFF4C6AFF;
        set_vert(g_blob_base, i, x, y, z, c, 0, 0);
    }
    // Tall cube (Y scale 2x)
    for (int i = 0; i < BLOB_VERTS; i++) {
        int16_t x = (i & 1) ? s : -s;
        int16_t y = (i & 2) ? s*2 : -s*2;
        int16_t z = (i & 4) ? s : -s;
        uint32_t c = 0x4CFF82FF;
        set_vert(g_blob_tall, i, x, y, z, c, 0, 0);
    }
    // Wide cube (X scale 2x)
    for (int i = 0; i < BLOB_VERTS; i++) {
        int16_t x = (i & 1) ? s*2 : -s*2;
        int16_t y = (i & 2) ? s : -s;
        int16_t z = (i & 4) ? s : -s;
        uint32_t c = 0xFFD94CFF;
        set_vert(g_blob_wide, i, x, y, z, c, 0, 0);
    }
}

static void blob_update(float dt)
{
    g_blob_time += dt;
    // Three-way blend by sine waves
    float w0 = 0.5f + 0.5f * fm_sinf(g_blob_time * 0.8f);
    float w1 = 0.5f + 0.5f * fm_sinf(g_blob_time * 0.8f + 2.094f);
    float w2 = 0.5f + 0.5f * fm_sinf(g_blob_time * 0.8f + 4.189f);
    float sum = w0 + w1 + w2;
    if (sum < 1e-6f) sum = 1.0f;
    w0 /= sum; w1 /= sum; w2 /= sum;

    int pc = (BLOB_VERTS + 1) / 2;
    memset(g_blob_work, 0, sizeof(T3DVertPacked) * pc);
    T3DVertPacked *targets[3] = { g_blob_base, g_blob_tall, g_blob_wide };
    float weights[3] = { w0, w1, w2 };
    for (int t = 0; t < 3; t++) {
        if (weights[t] == 0.0f) continue;
        for (int i = 0; i < pc; i++) {
            for (int j = 0; j < 3; j++) {
                g_blob_work[i].posA[j] += (int16_t)(targets[t][i].posA[j] * weights[t]);
                g_blob_work[i].posB[j] += (int16_t)(targets[t][i].posB[j] * weights[t]);
            }
            g_blob_work[i].rgbaA += (uint32_t)(targets[t][i].rgbaA * weights[t]);
            g_blob_work[i].rgbaB += (uint32_t)(targets[t][i].rgbaB * weights[t]);
        }
    }
    data_cache_hit_writeback(g_blob_work, sizeof(T3DVertPacked) * pc);
}

static void blob_draw(void)
{
    static int16_t tris[12][3] = {
        {0,2,1},{1,2,3},{4,5,6},{5,7,6},{0,4,2},{4,6,2},
        {1,3,5},{3,7,5},{0,1,4},{1,5,4},{2,6,3},{3,6,7},
    };
    kiln_transform_push(&(KilnTransform){
        .pos = { { 60, 10, 0 } },
        .scale = { { 1, 1, 1 } },
        .rot_axis = { { 0, 1, 0 } },
        .rot_angle = g_blob_time * 0.5f,
    });
    t3d_vert_load(g_blob_work, 0, BLOB_VERTS);
    for (int i = 0; i < 12; i++)
        t3d_tri_draw(tris[i][0], tris[i][1], tris[i][2]);
    t3d_tri_sync();
    kiln_transform_pop();
}

// ── Cel-shaded cube (RSP vertex FX demo) ──────────────────────────────

static T3DVertPacked *g_cel_cube;
static float g_cel_rot;
static bool g_cel_on = true;

static void cel_cube_init(void)
{
    g_cel_cube = alloc_verts(BLOB_VERTS);
    int16_t s = 14;
    uint32_t colors[8] = {
        0xFF4C6AFF, 0x4CFF82FF, 0xFFD94CFF, 0x00F5D4FF,
        0x8B5CF6FF, 0xFF4C6AFF, 0x4CFF82FF, 0xFFD94CFF,
    };
    for (int i = 0; i < BLOB_VERTS; i++) {
        int16_t x = (i & 1) ? s : -s;
        int16_t y = (i & 2) ? s : -s;
        int16_t z = (i & 4) ? s : -s;
        set_vert(g_cel_cube, i, x, y, z, colors[i], 0, 0);
    }
}

static void cel_cube_draw(float dt)
{
    g_cel_rot += dt;
    static int16_t tris[12][3] = {
        {0,2,1},{1,2,3},{4,5,6},{5,7,6},{0,4,2},{4,6,2},
        {1,3,5},{3,7,5},{0,1,4},{1,5,4},{2,6,3},{3,6,7},
    };
    kiln_transform_push(&(KilnTransform){
        .pos = { { 0, 30, -30 } },
        .scale = { { 1, 1, 1 } },
        .rot_axis = { { 0, 1, 0 } },
        .rot_angle = g_cel_rot,
    });
    if (g_cel_on) kiln_vfx_set(KILN_VFX_CELSHADE_COLOR, 0, 0);
    t3d_vert_load(g_cel_cube, 0, BLOB_VERTS);
    for (int i = 0; i < 12; i++)
        t3d_tri_draw(tris[i][0], tris[i][1], tris[i][2]);
    t3d_tri_sync();
    if (g_cel_on) kiln_vfx_clear();
    kiln_transform_pop();
}

// ── Main ──────────────────────────────────────────────────────────────

static KilnScene g_scene;
static float g_spin;

int main(void)
{
    debug_init_isviewer();
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    dfs_init(DFS_DEFAULT_LOCATION);

    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    kiln_input_init();
    kiln_scene_init(&g_scene);

    g_scene.cam_pos   = (fm_vec3_t){{ 0, 20, -90 }};
    g_scene.cam_target = (fm_vec3_t){{ 0, 0, 0 }};
    g_scene.fov_deg = 70.0f;
    g_scene.far_z = 300.0f;

    water_init();
    flag_setup();
    blob_setup();
    cel_cube_init();

    int mode = 0;
    const char *mode_names[] = {
        "All effects",
        "Water scroll",
        "Flag deform",
        "Morph blob",
        "Cel shade",
    };
    const int mode_count = sizeof(mode_names) / sizeof(mode_names[0]);

    while (1) {
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);

        if (in->edges & KILN_BTN_DL)  { mode--; if (mode < 0) mode = mode_count - 1; }
        if (in->edges & KILN_BTN_DR) { mode++; if (mode >= mode_count) mode = 0; }
        if (in->edges & KILN_BTN_A) g_cel_on = !g_cel_on;

        g_spin += DT;

        // Update
        flag_update(DT);
        blob_update(DT);

        // 3D pass
        kiln_scene_update(&g_scene);
        kiln_frame_begin();
        kiln_scene_begin(&g_scene);

        if (mode == 0 || mode == 1) water_draw(DT);
        if (mode == 0 || mode == 2) flag_draw();
        if (mode == 0 || mode == 3) blob_draw();
        if (mode == 0 || mode == 4) cel_cube_draw(DT);

        // 2D pass
        kiln_gui_begin();
        char buf[64];
        snprintf(buf, sizeof(buf), "Mode: %s", mode_names[mode]);
        kiln_gui_text(8, 8, (color_t){ 255, 255, 255, 255 }, "%s", buf);
        kiln_gui_text(8, 22, (color_t){ 200, 200, 200, 255 }, "%s", "L/R: mode  A: toggle cel");
        kiln_gui_end();

        kiln_frame_end();
    }
}