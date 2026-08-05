// SPDX-License-Identifier: MPL-2.0
//
// Open-world streaming demo: all five Phase E primitives in one ROM.
//
//   m64_scratch   — per-frame bump allocator for transform matrices
//   m64_cache     — refcounted cache for shared tile geometry
//   m64_tile      — tile residency manager (grid streaming + LOD)
//   m64_lod       — distance-based LOD selector
//   m64_twopass   — far (Z-off) + near (Z-on) two-pass renderer
//
// The world is a 16×16 grid of 64-unit tiles. Each tile is a flat coloured
// quad whose colour depends on its LOD level (bright = near, dim = far).
// The camera moves with the D-pad, and tiles stream in/out around it.
// The HUD shows the loaded tile count, cache entries, and scratch usage.

#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>

#include <m64/m64_engine.h>
#include <m64/m64_gui.h>
#include <m64/m64_scratch.h>
#include <m64/m64_cache.h>
#include <m64/m64_tile.h>
#include <m64/m64_lod.h>
#include <m64/m64_twopass.h>

#include <malloc.h>
#include <string.h>

#define SCREEN_W 320
#define SCREEN_H 240

/* World configuration. */
#define WORLD_TILES_X 16
#define WORLD_TILES_Y 16
#define TILE_SIZE    64.0f
#define WINDOW_TILES 2    /* half-size: 5×5 loaded window */
#define SLOTS_X      8    /* slot array (must be >= 2*WINDOW+1) */
#define SLOTS_Y      8

/* Each tile's user_data is a small struct with a colour + vertex buffer. */
typedef struct {
    color_t color;
    T3DVertPacked *verts;  /* 2 verts = 1 quad (degenerate) */
} TileData;

/* Per-LOD colours. */
static const color_t lod_colors[M64_TILE_MAX_LOD] = {
    { 255, 100, 100, 255 },  /* LOD 0: bright red */
    { 180,  80,  80, 255 },  /* LOD 1: medium red */
    { 120,  60,  60, 255 },  /* LOD 2: dim red */
};

/* Global state (single-player N64, module-global is fine). */
static M64Scratch scratch;
static M64Cache   cache;
static M64LODConfig lod_cfg;
static M64TileManager tiles;
static M64TileSlot  visual_slots[SLOTS_X * SLOTS_Y];
static M64Scene     scene;
static float cam_x = 512.0f, cam_z = 512.0f;
static float cam_angle = 0.0f;

/* ---- Tile load/unload callbacks ---- */

static void *tile_load(int16_t tx, int16_t ty, uint8_t lod, void *ctx)
{
    (void)ctx;
    TileData *td = malloc(sizeof(TileData));
    if (!td) return NULL;

    td->color = lod_colors[lod < M64_TILE_MAX_LOD ? lod : M64_TILE_MAX_LOD - 1];
    td->verts = malloc_uncached(sizeof(T3DVertPacked));
    if (!td->verts) { free(td); return NULL; }

    /* Build a flat quad for this tile. */
    float x0 = tx * TILE_SIZE;
    float z0 = ty * TILE_SIZE;
    float x1 = x0 + TILE_SIZE;
    float z1 = z0 + TILE_SIZE;
    int16_t s = (int16_t)(TILE_SIZE * 0.5f);

    fm_vec3_t n = {{ 0, 1, 0 }};
    uint32_t rgba = ((uint32_t)td->color.r << 24) |
                    ((uint32_t)td->color.g << 16) |
                    ((uint32_t)td->color.b << 8) | 0xFF;

    td->verts[0] = (T3DVertPacked){
        .posA = { -s, 0, -s }, .rgbaA = rgba, .normA = t3d_vert_pack_normal(&n),
        .posB = {  s, 0, -s }, .rgbaB = rgba, .normB = t3d_vert_pack_normal(&n),
    };
    /* A real tile would have 4+ verts; this is a degenerate 2-vert "tile"
     * for demonstration. The point is the streaming, not the geometry. */
    return td;
}

static void tile_unload(int16_t tx, int16_t ty, uint8_t lod,
                        void *user_data, void *ctx)
{
    (void)tx; (void)ty; (void)lod; (void)ctx;
    TileData *td = (TileData *)user_data;
    if (!td) return;
    if (td->verts) free(td->verts);
    free(td);
}

static void tile_sync(void *ctx)
{
    (void)ctx;
    /* No GPU sync needed in this simple demo — the tile geometry is
     * drawn synchronously within the frame. A real game with async
     * display lists would call rspq_wait() here. */
}

/* ---- Pass draw callback ---- */

static void pass_draw(M64TileGrid *grid, const M64LODConfig *lod,
                      int pass, M64Scratch *sc, const M64Scene *s,
                      void *ctx)
{
    (void)lod; (void)ctx;
    int far_threshold = M64_TWOPASS_FAR_LOD_THRESHOLD;

    for (M64TileSlot *slot = m64_tile_first(grid); slot;
         slot = m64_tile_next(grid, slot)) {
        TileData *td = (TileData *)slot->user_data;
        if (!td) continue;

        int is_far = (slot->lod >= far_threshold) ? 1 : 0;
        if (pass == 0 && !is_far) continue;  /* far pass: only far tiles */
        if (pass == 1 && is_far) continue;   /* near pass: only near tiles */

        /* Camera-relative transform. */
        float wx = slot->world_x * TILE_SIZE + TILE_SIZE * 0.5f - s->cam_pos.v[0];
        float wz = slot->world_y * TILE_SIZE + TILE_SIZE * 0.5f - s->cam_pos.v[2];

        T3DMat4FP *mtx = m64_scratch_mat4fp(sc);
        if (!mtx) continue;

        T3DMat4 m;
        t3d_mat4_identity(&m);
        t3d_mat4_translate(&m, wx, 0, wz);
        t3d_mat4_to_fixed_3x4(mtx, &m);

        t3d_matrix_push(mtx);
        t3d_vert_load(td->verts, 0, 2);
        t3d_tri_draw(0, 1, 1);  /* degenerate triangle (demo only) */
        t3d_tri_sync();
        t3d_matrix_pop(1);
    }
}

/* ---- Main ---- */

int main(void)
{
    /* Init engine. */
    m64_engine_init(RESOLUTION_320x240);
    m64_scene_init(&scene);
    scene.cam_pos = (fm_vec3_t){{ 512, 100, 512 }};
    scene.cam_target = (fm_vec3_t){{ 512, 0, 512 }};
    scene.fov_deg = 70;
    scene.far_z = 600;

    /* Init scratch allocator. */
    m64_scratch_init(&scratch);

    /* Init resource cache. */
    m64_cache_init(&cache);

    /* Init LOD config. */
    m64_lod_init_defaults(&lod_cfg, TILE_SIZE);

    /* Init tile manager. */
    M64TileGridConfig cfg = {
        .tile_count_x = WORLD_TILES_X,
        .tile_count_y = WORLD_TILES_Y,
        .tile_size = TILE_SIZE,
        .origin = {{ 0, 0, 0 }},
        .window_tiles = WINDOW_TILES,
        .slots_x = SLOTS_X,
        .slots_y = SLOTS_Y,
    };
    m64_tile_init(&tiles, &cfg, visual_slots,
                  NULL, NULL,  /* no collision grid for this demo */
                  tile_load, tile_unload, tile_sync, NULL);

    /* Main loop. */
    joypad_init();
    rdpq_init();

    while (1) {
        joypad_poll();
        joypad_buttons_t btn = joypad_get_buttons_pressed(JOYPAD_PORT_1);
        joypad_inputs_t inp = joypad_get_inputs(JOYPAD_PORT_1);

        /* Move camera with stick. */
        if (btn.c_right) cam_angle += 0.05f;
        if (btn.c_left)  cam_angle -= 0.05f;
        cam_x += (float)inp.stick_x * 0.1f;
        cam_z -= (float)inp.stick_y * 0.1f;

        /* Clamp to world. */
        if (cam_x < 0) cam_x = 0;
        if (cam_z < 0) cam_z = 0;
        if (cam_x > WORLD_TILES_X * TILE_SIZE) cam_x = WORLD_TILES_X * TILE_SIZE;
        if (cam_z > WORLD_TILES_Y * TILE_SIZE) cam_z = WORLD_TILES_Y * TILE_SIZE;

        scene.cam_pos = (fm_vec3_t){{ cam_x, 100, cam_z }};
        scene.cam_target = (fm_vec3_t){{ cam_x, 0, cam_z + 100 }};

        /* Per-frame: flush unload queue from last frame, then update. */
        m64_tile_flush_unload(&tiles);
        m64_tile_update(&tiles, scene.cam_pos, m64_lod_selector_cb);
        m64_scratch_begin(&scratch);
        m64_scene_update(&scene);

        /* Render. */
        m64_frame_begin();
        m64_scene_begin(&scene);

        /* Two-pass tile rendering. */
        m64_twopass_render(&tiles.visual, &lod_cfg, &scratch, &scene,
                           pass_draw, NULL);

        m64_gui_begin();

        /* HUD. */
        char buf[192];
        uint16_t loaded = 0;
        for (M64TileSlot *s = m64_tile_first(&tiles.visual); s;
             s = m64_tile_next(&tiles.visual, s)) loaded++;

        snprintf(buf, sizeof(buf), "Tiles: %d  Scratch: %u/%d  LOD0-2",
                 loaded, scratch.offset, M64_SCRATCH_SIZE);
        rdpq_text_print(NULL, 0, 10, 10, buf);

        snprintf(buf, sizeof(buf), "Cam: (%.0f, %.0f)", cam_x, cam_z);
        rdpq_text_print(NULL, 0, 10, 20, buf);

        m64_gui_end();
        m64_frame_end();
    }

    return 0;
}