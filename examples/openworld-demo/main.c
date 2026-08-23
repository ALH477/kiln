// SPDX-License-Identifier: MIT
//
// Open-world streaming demo: all five Phase E primitives, plus Phase F's
// kiln_stream + kiln_streamio pacer:
//
//   kiln_scratch   — per-frame bump allocator for transform matrices
//   kiln_cache     — refcounted cache for shared tile geometry
//   kiln_tile      — tile residency manager (grid streaming + LOD)
//   kiln_lod       — distance-based LOD selector
//   kiln_twopass   — far (Z-off) + near (Z-on) two-pass renderer
//   kiln_stream    — priority/budget admission for the loads below
//   kiln_streamio  — binds kiln_stream to real kiln_asset + kiln_cache loads
//
// This used to build its "tiles" out of a hand-malloc'd 2-vert stub with no
// real asset behind it — every kiln_tile consumer in this repo still does.
// Now tile_on_load submits a kiln_stream_request instead of loading
// anything itself; kiln_streamio_pump issues the real kiln_asset_model +
// kiln_cache_acquire call later the same frame, only for requests the
// pacer actually admitted under this frame's byte/count budget, in
// distance/urgency order. tiles.load_budget is set to 255 so kiln_tile's
// OWN throttling never engages — kiln_stream is the only budget authority
// (see kiln_streamio.h's file comment for why that is safe).
//
// The world is a 16×16 grid of 64-unit tiles, each drawing the same shared
// StreamDB-packed model (openworld.streamdb: models/tile.t3dm) — the point
// of this demo is the pacer's priority ordering across many simultaneous
// tile requests, not per-tile unique geometry. The camera moves with the
// D-pad; the HUD reports loaded tile count, cache entries, scratch usage,
// and the pacer's two health gauges (dropped/failed), which should both
// read zero in a normal run.

#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>

#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_scratch.h>
#include <kiln/kiln_cache.h>
#include <kiln/kiln_tile.h>
#include <kiln/kiln_lod.h>
#include <kiln/kiln_twopass.h>
#include <kiln/kiln_asset.h>
#include <kiln/kiln_streamio.h>

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
#define SLOT_COUNT   (SLOTS_X * SLOTS_Y)

/* Global state (single-player N64, module-global is fine). */
static KilnScratch scratch;
static KilnCache   cache;
static KilnLODConfig lod_cfg;
static KilnTileManager tiles;
static KilnTileSlot  visual_slots[SLOT_COUNT];
static KilnScene     scene;
static float cam_x = 512.0f, cam_z = 512.0f;
static float cam_angle = 0.0f;

/* The streaming pacer: one shared asset DB + cache, bound through
 * kiln_streamio to the tile grid above. */
static KilnAsset *g_db;
static KilnStreamIO g_io;
static KilnStreamIOTileBinding g_tile_binding;
static KilnStreamIOSlot g_tile_io_slots[SLOT_COUNT];

/* Every tile shares the same model key — see the file comment. */
static void tile_key(int16_t tx, int16_t ty, uint8_t lod, char *out, size_t out_len)
{
    (void)tx; (void)ty; (void)lod;
    snprintf(out, out_len, "models/tile.t3dm");
}

static void tile_sync(void *ctx)
{
    (void)ctx;
    /* No GPU sync needed in this simple demo — the tile geometry is
     * drawn synchronously within the frame. A real game with async
     * display lists would call rspq_wait() here. */
}

/* kiln_lod_selector_cb expects `user_ctx` to BE a KilnLODConfig*, but
 * KilnTileManager has only one user_ctx slot, and this demo needs it for
 * kiln_streamio's tile binding (&g_tile_binding) instead. Reading the
 * module-global lod_cfg directly sidesteps the conflict — the same fix a
 * game with its own tile-load context would need. (The demo's ORIGINAL
 * user_ctx was NULL here, which kiln_lod_selector_cb dereferenced as a
 * KilnLODConfig* — every tile's distance-vs-threshold compare then read off
 * a NULL cfg, came back beyond the last threshold, and no tile was ever
 * loaded. Streaming or not, that bug pre-dates this file's pacer wiring.) */
static uint8_t lod_select_from_global(int16_t tx, int16_t ty, float dist_sq, void *ctx)
{
    (void)tx; (void)ty; (void)ctx;
    return kiln_lod_select(&lod_cfg, dist_sq);
}

/* ---- Pass draw callback ---- */

static void pass_draw(KilnTileGrid *grid, const KilnLODConfig *lod,
                      int pass, KilnScratch *sc, const KilnScene *s,
                      void *ctx)
{
    (void)lod; (void)ctx;
    int far_threshold = KILN_TWOPASS_FAR_LOD_THRESHOLD;

    for (KilnTileSlot *slot = kiln_tile_first(grid); slot;
         slot = kiln_tile_next(grid, slot)) {
        /* NULL means the load is still outstanding — kiln_streamio_pump
         * hasn't resolved it yet, or it's still waiting on the pacer's
         * budget. Skipping it is the same null-check every kiln_tile/
         * kiln_room consumer already needs for a tile that has no visible
         * geometry yet. */
        T3DModel *model = (T3DModel *)slot->user_data;
        if (!model) continue;

        int is_far = (slot->lod >= far_threshold) ? 1 : 0;
        if (pass == 0 && !is_far) continue;  /* far pass: only far tiles */
        if (pass == 1 && is_far) continue;   /* near pass: only near tiles */

        /* World-space transform. kiln_scene_update's t3d_viewport_look_at
         * already takes s->cam_pos as an absolute WORLD position, so tile
         * geometry belongs at its own world coordinates — subtracting
         * cam_pos here (the demo's original code did, unconditionally)
         * double-counts the camera offset and pushes every tile roughly
         * cam_pos units further from wherever the camera is actually
         * looking, well outside far_z. Pre-existing, independent of the
         * streaming pacer this file now also exercises. */
        float wx = slot->world_x * TILE_SIZE + TILE_SIZE * 0.5f;
        float wz = slot->world_y * TILE_SIZE + TILE_SIZE * 0.5f;

        T3DMat4FP *mtx = kiln_scratch_mat4fp(sc);
        if (!mtx) continue;

        T3DMat4 m;
        t3d_mat4_identity(&m);
        t3d_mat4_translate(&m, wx, 0, wz);
        t3d_mat4_to_fixed_3x4(mtx, &m);

        t3d_matrix_push(mtx);
        t3d_model_draw(model);
        t3d_matrix_pop(1);
    }
}

/* ---- Main ---- */

int main(void)
{
    /* Init engine. */
    kiln_engine_init(RESOLUTION_320x240);
    kiln_scene_init(&scene);
    scene.cam_pos = (fm_vec3_t){{ 512, 100, 512 }};
    scene.cam_target = (fm_vec3_t){{ 512, 0, 512 }};
    scene.fov_deg = 70;
    scene.far_z = 600;

    /* Init scratch allocator. */
    kiln_scratch_init(&scratch);

    /* Init resource cache. */
    kiln_cache_init(&cache);

    /* Init LOD config. */
    kiln_lod_init_defaults(&lod_cfg, TILE_SIZE);

    /* Open the StreamDB and bind the streaming pacer to it. A missing DB is
     * a content fact (wrong ROM), not a programming error, but this demo
     * has nothing to show without it. */
    dfs_init(DFS_DEFAULT_LOCATION);
    size_t need = kiln_asset_probe_size("rom:/openworld.streamdb");
    assertf(need > 0, "openworld-demo: openworld.streamdb not found / probe failed");
    void *arena = malloc(need);
    assertf(arena, "openworld-demo: arena malloc %zu failed", need);
    g_db = kiln_asset_open("rom:/openworld.streamdb", arena, need);
    assertf(g_db, "openworld-demo: kiln_asset_open failed");

    /* 2 admits/frame, 8 KB/frame: deliberately tight enough that scrolling
     * fast produces visible PENDING tiles, so the pacer's deferral is
     * actually exercised rather than trivially satisfied every frame. */
    KilnStreamBudget budget = { .max_admits_per_frame = 2, .max_bytes_per_frame = 8192 };
    kiln_streamio_init(&g_io, g_db, &cache, budget);

    /* Init tile manager. */
    KilnTileGridConfig cfg = {
        .tile_count_x = WORLD_TILES_X,
        .tile_count_y = WORLD_TILES_Y,
        .tile_size = TILE_SIZE,
        .origin = {{ 0, 0, 0 }},
        .window_tiles = WINDOW_TILES,
        .slots_x = SLOTS_X,
        .slots_y = SLOTS_Y,
    };

    /* kiln_tile_init first: it populates tiles.visual.cfg (slots_x/slots_y),
     * which kiln_streamio_tile_bind needs to size and index g_tile_io_slots.
     * Binding before init would size the slot table off a zeroed cfg. */
    kiln_tile_init(&tiles, &cfg, visual_slots,
                  NULL, NULL,  /* no collision grid for this demo */
                  kiln_streamio_tile_on_load, kiln_streamio_tile_on_unload,
                  tile_sync, &g_tile_binding);
    kiln_streamio_tile_bind(&g_tile_binding, &g_io, &tiles.visual, g_tile_io_slots,
                            KILN_STREAMIO_MODEL, tile_key);
    /* kiln_tile's own load_budget must be disabled (255 = load-all-
     * immediately) so kiln_stream is the ONLY budget authority — see
     * kiln_streamio.h's file comment for why this is safe: on_load does no
     * real I/O, it only enqueues a request. */
    tiles.load_budget = 255;

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

        /* Per-frame: flush unload queue from last frame, update residency
         * (this issues cheap kiln_stream_request calls only), then let the
         * pacer admit and actually load under budget. */
        g_tile_binding.camera_pos = scene.cam_pos;
        kiln_tile_flush_unload(&tiles);
        kiln_tile_update(&tiles, scene.cam_pos, lod_select_from_global);
        kiln_streamio_pump(&g_io);

        kiln_scratch_begin(&scratch);
        kiln_scene_update(&scene);

        /* Render. */
        kiln_frame_begin();
        kiln_scene_begin(&scene);

        /* Two-pass tile rendering. */
        kiln_twopass_render(&tiles.visual, &lod_cfg, &scratch, &scene,
                           pass_draw, NULL);

        kiln_gui_begin();

        /* HUD. Routed through kiln_gui_text (font id KILN_GUI_FONT,
         * registered by kiln_engine_init's kiln_gui_init call) rather than
         * a raw rdpq_text_print(NULL, 0, ...) — font id 0 is reserved and
         * asserts, which this demo's original HUD calls never actually hit
         * before now because nothing had booted this ROM to find out. */
        uint16_t loaded = 0;
        for (KilnTileSlot *s = kiln_tile_first(&tiles.visual); s;
             s = kiln_tile_next(&tiles.visual, s)) loaded++;

        color_t hud_color = RGBA32(232, 232, 240, 255);
        kiln_gui_text(10, 10, hud_color, "Tiles: %d  Scratch: %lu/%d  LOD0-2",
                     loaded, (unsigned long)scratch.offset, KILN_SCRATCH_SIZE);
        kiln_gui_text(10, 20, hud_color, "Cam: (%.0f, %.0f)", cam_x, cam_z);

        /* Pacer gauges: both should read 0 in a healthy run. dropped is a
         * capacity/tuning problem (pool too small); failed is a content
         * problem (bad key / missing StreamDB entry) — see kiln_stream.h /
         * kiln_streamio.h for why they're tracked separately. */
        kiln_gui_text(10, 30, hud_color, "Stream: pending %u  dropped %lu  failed %lu",
                     kiln_stream_pending_count(&g_io.stream),
                     (unsigned long)kiln_stream_dropped_total(&g_io.stream),
                     (unsigned long)kiln_streamio_fail_total(&g_io));

        kiln_gui_end();
        kiln_frame_end();
    }

    return 0;
}
