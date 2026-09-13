// SPDX-License-Identifier: MIT
//
// Open-world streaming demo: a 32x32-tile island, streamed as you fly over it.
//
//   kiln_tile      residency: a 7x7 window of tiles round the focus point
//   kiln_lod       three LODs, chosen by distance from the CAMERA
//   kiln_stream    the pacer: which outstanding loads run this frame, under a
//                  count and byte budget, nearest first
//   kiln_streamio  binds that pacer to real kiln_asset + kiln_cache loads
//   kiln_cache     one resident copy per model key, refcounted across tiles
//   kiln_twopass   far tiles with Z off (sorted back to front), near with Z on
//   kiln_scratch   per-frame tile matrices
//
// ── The world ──────────────────────────────────────────────────────────
// Each tile has a height and a biome (water, grass, forest, rock) from a
// value-noise height field masked into an island, computed once at boot. The
// tile's model is `models/ow_<biome>_<lod>.t3dm` out of openworld.streamdb —
// twelve models from tools/blender/ow_tile.py, where LOD 0 has trees, bushes
// and boulders, LOD 1 a few, and LOD 2 is a bare convex block. Every tile also
// turns by a hashed multiple of 90 degrees so repeats read less.
//
// A tile whose load has not been admitted yet draws a flat PROXY block in its
// biome colour instead of a hole: that is the pacer's deferral made visible
// without making the world look broken. Z shows each tile's LOD as an outline.
//
// ── Why LOD is measured from the camera ────────────────────────────────
// kiln_tile hands the selector the distance from the FOCUS, which sits ahead of
// the camera. kiln_twopass draws LOD 2 with the Z-buffer off, first, and lets
// nearer tiles overwrite it — so a far-LOD tile must genuinely be farther from
// the eye than every near-LOD one. Tiles behind the focus are close to the
// camera; by focus distance they would be "far" and get painted over by tiles
// that are actually behind them.
//
// ── Fog is the edge ────────────────────────────────────────────────────
// The window is 7 tiles across. With a 50 degree field of view its side edges
// only enter the frame past ~540 units of depth, which is where the fog closes
// on the console (see the fog note in main), and the sea plane under the
// island shares the fog colour, so there is no seam.
//
// Controls: stick moves, A runs, C-left/right orbit, C-up/down raise/lower the
// camera, Z toggles the LOD overlay. Three idle seconds and the autopilot flies
// a Lissajous figure over the island. Jump ROM FAST (.#openworld-demo-fast):
// the autopilot at 3.5x, so the pacer runs behind and its gauges earn their red.

#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>
#include <t3d/t3dmodel.h>

#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_prim.h>
#include <kiln/kiln_debugdraw.h>
#include <kiln/kiln_scratch.h>
#include <kiln/kiln_cache.h>
#include <kiln/kiln_tile.h>
#include <kiln/kiln_lod.h>
#include <kiln/kiln_twopass.h>
#include <kiln/kiln_asset.h>
#include <kiln/kiln_streamio.h>

#include <malloc.h>
#include <string.h>

enum { JUMP_NONE, JUMP_FAST };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W 320
#define SCREEN_H 240
#define DT (1.0f / 60.0f)

#define WORLD_N      32
#define TILE_SIZE    96.0f
#define WORLD_SIZE   (WORLD_N * TILE_SIZE)
#define WINDOW_TILES 3          /* half-size: a 7x7 window */
#define SLOTS_X      8          /* must be >= 2 * WINDOW + 1 */
#define SLOTS_Y      8
#define SLOT_COUNT   (SLOTS_X * SLOTS_Y)

#define LOD0_R 360.0f           /* horizontal distance from the camera */
#define LOD1_R 540.0f

#define STREAM_ADMITS 3
#define STREAM_BYTES  8192
#define PENDING_RED   24        /* a backlog this deep is visibly behind */
#define IDLE_FRAMES   180

enum { B_WATER, B_GRASS, B_FOREST, B_ROCK, B_COUNT };
static const char *const BIOME_KEY[B_COUNT] = { "water", "grass", "forest", "rock" };
static const uint8_t LEVEL_HEIGHT[7] = { 0, 20, 44, 72, 104, 144, 192 };

static uint8_t g_biome[WORLD_N][WORLD_N];   /* [ty][tx] */
static uint8_t g_level[WORLD_N][WORLD_N];
static uint8_t g_turn[WORLD_N][WORLD_N];

static KilnScratch     g_scratch;
static KilnCache       g_cache;
static KilnLODConfig   g_lod;
static KilnTileManager g_tiles;
static KilnTileSlot    g_slots[SLOT_COUNT];
static KilnScene       g_scene;

static KilnAsset              *g_db;
static KilnStreamIO            g_io;
static KilnStreamIOTileBinding g_bind;
static KilnStreamIOSlot        g_io_slots[SLOT_COUNT];

static KilnPrim g_proxy[B_COUNT];
static KilnPrim g_sea;

/* ── The height field ─────────────────────────────────────────────────── */
static uint32_t hash2(int x, int y, uint32_t seed)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static float lattice(int x, int y, uint32_t seed)
{
    return (float)(hash2(x, y, seed) & 0xFFFF) / 65535.0f;
}

static float value_noise(float x, float y, uint32_t seed)
{
    const int xi = (int)x, yi = (int)y;
    float fx = x - (float)xi, fy = y - (float)yi;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    const float a = lattice(xi, yi, seed), b = lattice(xi + 1, yi, seed);
    const float c = lattice(xi, yi + 1, seed), d = lattice(xi + 1, yi + 1, seed);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

static void build_world(void)
{
    const float centre = (WORLD_N - 1) * 0.5f;
    for (int ty = 0; ty < WORLD_N; ty++) {
        for (int tx = 0; tx < WORLD_N; tx++) {
            const float n = 0.62f * value_noise(tx / 5.3f, ty / 5.3f, 11) +
                            0.38f * value_noise(tx / 2.2f, ty / 2.2f, 29);
            const float dx = (float)tx - centre, dy = (float)ty - centre;
            const fm_vec3_t off = {{ dx, dy, 0.0f }};
            float mask = 1.25f - fm_vec3_len(&off) / 13.0f;
            mask = mask < 0 ? 0 : mask > 1 ? 1 : mask;
            int level = (int)(n * mask * 1.35f * 7.0f);
            if (level > 6) level = 6;
            g_level[ty][tx] = (uint8_t)level;
            const uint32_t h = hash2(tx, ty, 5);
            g_biome[ty][tx] = level == 0 ? B_WATER
                            : level <= 2 ? B_GRASS
                            : level <= 4 ? ((h % 4) ? B_FOREST : B_GRASS)
                            : B_ROCK;
            g_turn[ty][tx] = (uint8_t)((h >> 8) & 3);
        }
    }
}

static float tile_height(int tx, int ty) { return LEVEL_HEIGHT[g_level[ty][tx]]; }

/* kiln_tile_first/next also yield tiles already queued for unload (kiln_tile.h's
 * flags bit 1): they leave the window this frame and are freed at the next
 * flush. Counting or drawing them read as "tiles 56/49" whenever the window
 * moved. */
#define TILE_FLAG_UNLOAD_PENDING 0x02
static int tile_live(const KilnTileSlot *s) { return !(s->flags & TILE_FLAG_UNLOAD_PENDING); }

static float ground_at(float x, float z)
{
    int tx = (int)(x / TILE_SIZE), ty = (int)(z / TILE_SIZE);
    if (tx < 0) tx = 0;
    if (ty < 0) ty = 0;
    if (tx >= WORLD_N) tx = WORLD_N - 1;
    if (ty >= WORLD_N) ty = WORLD_N - 1;
    return tile_height(tx, ty);
}

/* ── Streaming callbacks ──────────────────────────────────────────────── */
static void tile_key(int16_t tx, int16_t ty, uint8_t lod, char *out, size_t out_len)
{
    snprintf(out, out_len, "models/ow_%s_%u.t3dm", BIOME_KEY[g_biome[ty][tx]], (unsigned)lod);
}

static void tile_sync(void *ctx) { (void)ctx; rspq_wait(); }

/* See the file comment: LOD by horizontal distance from the camera. */
static uint8_t lod_from_camera(int16_t tx, int16_t ty, float dist_sq, void *ctx)
{
    (void)dist_sq; (void)ctx;
    const float dx = (tx + 0.5f) * TILE_SIZE - g_scene.cam_pos.v[0];
    const float dz = (ty + 0.5f) * TILE_SIZE - g_scene.cam_pos.v[2];
    return kiln_lod_select(&g_lod, dx * dx + dz * dz);
}

/* ── Drawing ──────────────────────────────────────────────────────────── */
static int tile_visible(const KilnScene *s, float wx, float h, float wz)
{
    return kiln_scene_depth(s, (fm_vec3_t){{ wx, h, wz }}) > -TILE_SIZE;
}

static void draw_tile(const KilnTileSlot *slot, KilnScratch *sc)
{
    const int tx = slot->world_x, ty = slot->world_y;
    const float wx = (tx + 0.5f) * TILE_SIZE, wz = (ty + 0.5f) * TILE_SIZE;
    const float h = tile_height(tx, ty);
    T3DMat4FP *mtx = kiln_scratch_mat4fp(sc);
    if (!mtx) return;

    static const float C[4] = { 1, 0, -1, 0 }, S[4] = { 0, 1, 0, -1 };
    const T3DModel *model = (const T3DModel *)slot->user_data;
    const int k = model ? g_turn[ty][tx] : 0;
    fm_mat4_t m = {0};
    m.m[0][0] = C[k];  m.m[0][2] = -S[k];
    m.m[1][1] = 1.0f;
    m.m[2][0] = S[k];  m.m[2][2] = C[k];
    m.m[3][0] = wx;    m.m[3][1] = h;     m.m[3][2] = wz;   m.m[3][3] = 1.0f;
    t3d_mat4_to_fixed_3x4(mtx, &m);

    t3d_matrix_push(mtx);
    if (model) {
        t3d_model_draw((T3DModel *)model);
    } else {
        /* Still waiting on the pacer: the biome's proxy block. */
        t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_CULL_BACK);
        kiln_prim_draw(&g_proxy[g_biome[ty][tx]]);
    }
    t3d_matrix_pop(1);
}

typedef struct { KilnTileSlot *slot; float d2; } FarTile;

static void pass_draw(KilnTileGrid *grid, const KilnLODConfig *lod, int pass,
                      KilnScratch *sc, const KilnScene *s, void *ctx)
{
    (void)lod; (void)ctx;
    FarTile far[SLOT_COUNT];
    int nfar = 0;

    for (KilnTileSlot *slot = kiln_tile_first(grid); slot; slot = kiln_tile_next(grid, slot)) {
        if (!tile_live(slot)) continue;
        const int is_far = slot->lod >= KILN_TWOPASS_FAR_LOD_THRESHOLD;
        if (is_far != (pass == 0)) continue;
        const float wx = (slot->world_x + 0.5f) * TILE_SIZE;
        const float wz = (slot->world_y + 0.5f) * TILE_SIZE;
        if (!tile_visible(s, wx, tile_height(slot->world_x, slot->world_y), wz)) continue;
        if (pass == 1) {
            draw_tile(slot, sc);
            continue;
        }
        /* The far pass has no depth test: painter's order, far to near. */
        const float dx = wx - s->cam_pos.v[0], dz = wz - s->cam_pos.v[2];
        const float d2 = dx * dx + dz * dz;
        int i = nfar++;
        while (i > 0 && far[i - 1].d2 < d2) { far[i] = far[i - 1]; i--; }
        far[i] = (FarTile){ slot, d2 };
    }
    for (int i = 0; i < nfar; i++) draw_tile(far[i].slot, sc);
}

/* ── Minimap: biome runs baked at boot, residency drawn over them live ─── */
#define MAP_PX 2
#define MAP_X  (SCREEN_W - 8 - 66)
#define MAP_Y  (SCREEN_H - 8 - 66)
typedef struct { uint8_t x, y, w, biome; } MapRun;
static MapRun g_runs[WORLD_N * WORLD_N];
static int g_run_count;

static const color_t BIOME_MAP[B_COUNT] = {
    { 0x2C, 0x50, 0x78, 0xFF }, { 0x48, 0x78, 0x38, 0xFF },
    { 0x2C, 0x5C, 0x2C, 0xFF }, { 0x78, 0x76, 0x70, 0xFF },
};

/* World +Z is up on the map and world +X is LEFT, which is how it looks from
 * above with +Z ahead (screen-right is -X). */
static int map_x(int tx) { return MAP_X + 1 + (WORLD_N - 1 - tx) * MAP_PX; }
static int map_y(int ty) { return MAP_Y + 1 + (WORLD_N - 1 - ty) * MAP_PX; }

static void build_map_runs(void)
{
    g_run_count = 0;
    for (int ty = 0; ty < WORLD_N; ty++) {
        int tx = 0;
        while (tx < WORLD_N) {
            const uint8_t b = g_biome[ty][tx];
            int end = tx;
            while (end + 1 < WORLD_N && g_biome[ty][end + 1] == b) end++;
            /* Stored in map order: the run's LEFT edge is its highest tx. */
            g_runs[g_run_count++] = (MapRun){ (uint8_t)(WORLD_N - 1 - end), (uint8_t)(WORLD_N - 1 - ty),
                                              (uint8_t)(end - tx + 1), b };
            tx = end + 1;
        }
    }
}

/* ── HUD ──────────────────────────────────────────────────────────────── */
static const color_t INK   = { 0xEC, 0xEC, 0xF2, 0xFF };
static const color_t DIM   = { 0x98, 0xA4, 0xB8, 0xFF };
static const color_t TEAL  = { 0x00, 0xF0, 0xD0, 0xFF };
static const color_t RED   = { 0xFF, 0x48, 0x48, 0xFF };
static const color_t AMBER = { 0xFF, 0xB8, 0x40, 0xFF };
static const color_t PANEL = { 0x10, 0x16, 0x24, 0xFF };
static const color_t LOD_COL[3] = { { 0xFF, 0xFF, 0xFF, 0xFF }, { 0xFF, 0xE0, 0x70, 0xFF },
                                    { 0xFF, 0x90, 0x38, 0xFF } };

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);
    kiln_input_init();

    build_world();
    build_map_runs();

    kiln_scene_init(&g_scene);
    /* Fog 160..280 is chosen in Ares. Tiny3D ramps fog over CLIP z, not view
     * depth: fog = (clip_z - 2*near) / (2*(far - near)), with clip_z =
     * f*(d - 2n)/(f - n) for the camera's own near/far planes, so the range
     * closes well beyond 280 of depth and moves with the camera's near plane.
     * plat/host computes the same ramp now (kiln-prim pins it against probe
     * measurements), so a host render of this demo shows the island as the
     * ROM does. */
    kiln_prim_stage(&g_scene, RGBA32(0x9C, 0xB8, 0xD4, 0xFF), 160.0f, 280.0f);
    /* Sun and sky light, set explicitly. A direction points TOWARD the
     * source (Tiny3D lights by +dot(normal, dir)): a high sun, a weaker cool
     * fill from the opposite side. */
    g_scene.light_dir = (fm_vec3_t){{ 0.40f, 0.80f, -0.45f }};
    fm_vec3_norm(&g_scene.light_dir, &g_scene.light_dir);
    g_scene.lights[0].dir = (fm_vec3_t){{ -0.60f, 0.35f, 0.70f }};
    fm_vec3_norm(&g_scene.lights[0].dir, &g_scene.lights[0].dir);
    g_scene.fov_deg = 50.0f;
    g_scene.near_z = 16.0f;
    g_scene.far_z = 660.0f;

    /* Proxies: the block a tile draws until its model is admitted. */
    static const uint8_t PROXY_RGB[B_COUNT][3] = {
        { 0x3C, 0x7C, 0xB0 }, { 0x6C, 0xAC, 0x50 }, { 0x4C, 0x88, 0x44 }, { 0xA0, 0x9C, 0x94 },
    };
    for (int b = 0; b < B_COUNT; b++) {
        const uint32_t top = kiln_prim_rgba(PROXY_RGB[b][0], PROXY_RGB[b][1], PROXY_RGB[b][2]);
        kiln_prim_box(&g_proxy[b], (fm_vec3_t){{ 0, -104, 0 }}, (fm_vec3_t){{ 48, 104, 48 }},
                      top, kiln_prim_shade(top, 0.6f), kiln_prim_shade(top, 0.4f));
    }
    kiln_prim_floor(&g_sea, 720.0f, 18, kiln_prim_rgba(0x3A, 0x74, 0xA8), kiln_prim_rgba(0x36, 0x6E, 0xA2));
    KilnTransform sea_xf;
    kiln_transform_init(&sea_xf);

    kiln_scratch_init(&g_scratch);
    kiln_cache_init(&g_cache);
    g_lod.thresholds_sq[0] = LOD0_R * LOD0_R;
    g_lod.thresholds_sq[1] = LOD1_R * LOD1_R;
    g_lod.thresholds_sq[2] = 4000.0f * 4000.0f;   /* the window, not the LOD, bounds it */
    g_lod.threshold_count = 3;

    const size_t need = kiln_asset_probe_size("rom:/openworld.streamdb");
    assertf(need > 0, "openworld-demo: openworld.streamdb not found / probe failed");
    void *arena = malloc(need);
    assertf(arena, "openworld-demo: arena malloc %u failed", (unsigned)need);
    g_db = kiln_asset_open("rom:/openworld.streamdb", arena, need);
    assertf(g_db, "openworld-demo: kiln_asset_open failed");

    kiln_streamio_init(&g_io, g_db, &g_cache,
                       (KilnStreamBudget){ .max_admits_per_frame = STREAM_ADMITS,
                                           .max_bytes_per_frame = STREAM_BYTES });

    const KilnTileGridConfig cfg = {
        .tile_count_x = WORLD_N, .tile_count_y = WORLD_N, .tile_size = TILE_SIZE,
        .origin = {{ 0, 0, 0 }}, .window_tiles = WINDOW_TILES,
        .slots_x = SLOTS_X, .slots_y = SLOTS_Y,
    };
    /* kiln_tile_init first: it fills tiles.visual.cfg, which the binding
     * sizes its slot table from. */
    kiln_tile_init(&g_tiles, &cfg, g_slots, NULL, NULL,
                   kiln_streamio_tile_on_load, kiln_streamio_tile_on_unload,
                   tile_sync, &g_bind);
    kiln_streamio_tile_bind(&g_bind, &g_io, &g_tiles.visual, g_io_slots,
                            KILN_STREAMIO_MODEL, tile_key);
    g_tiles.load_budget = 255;   /* kiln_stream is the only budget authority */

    /* ── Flight state ─────────────────────────────────────────────────── */
    const float ap_rate = KILN_JUMP == JUMP_FAST ? 3.5f : 1.0f;
    fm_vec3_t focus = {{ WORLD_SIZE * 0.5f, 0, WORLD_SIZE * 0.5f }};
    float yaw = 0.0f;               /* camera heading: looks along (sin, 0, cos) */
    float cam_height = 290.0f;
    float ground = 0.0f;
    float ap_t = 0.0f;
    int autopilot = KILN_JUMP == JUMP_FAST;
    int idle = 0;
    int overlay = 0;

    uint32_t frames = 0;
    uint32_t bytes_peak = 0, bytes_hold = 0;
    float fps = 60.0f;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);
        const int touched = in->buttons != 0 ||
                            in->stick_x * in->stick_x + in->stick_y * in->stick_y > 0.02f;
        if (in->edges & KILN_BTN_Z) overlay = !overlay;
        if (KILN_JUMP == JUMP_NONE) {
            idle = touched ? 0 : idle + 1;
            if (touched) autopilot = 0;
            else if (idle >= IDLE_FRAMES) autopilot = 1;
        }

        if (++frames % 30 == 0) {
            const uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        if (autopilot) {
            /* A Lissajous figure over the island; ease onto it rather than
             * teleporting from wherever the player left the focus. */
            ap_t += DT * ap_rate;
            const float c = WORLD_SIZE * 0.5f, a = c - 300.0f;
            const fm_vec3_t want = {{ c + a * fm_sinf(0.13f * ap_t), 0,
                                      c + a * fm_sinf(0.21f * ap_t + 1.1f) }};
            const float vx = a * 0.13f * fm_cosf(0.13f * ap_t);
            const float vz = a * 0.21f * fm_cosf(0.21f * ap_t + 1.1f);
            const float k = idle > IDLE_FRAMES + 120 || KILN_JUMP == JUMP_FAST ? 1.0f : 2.0f * DT;
            focus.v[0] += (want.v[0] - focus.v[0]) * k;
            focus.v[2] += (want.v[2] - focus.v[2]) * k;
            float dyaw = fm_atan2f(vx, vz) - yaw;
            while (dyaw > 3.14159f) dyaw -= 6.28318f;
            while (dyaw < -3.14159f) dyaw += 6.28318f;
            yaw += dyaw * (2.5f * DT);
        } else {
            if (in->buttons & KILN_BTN_CL) yaw += 1.8f * DT;
            if (in->buttons & KILN_BTN_CR) yaw -= 1.8f * DT;
            if (in->buttons & KILN_BTN_CU) cam_height -= 120.0f * DT;
            if (in->buttons & KILN_BTN_CD) cam_height += 120.0f * DT;
            cam_height = cam_height < 160.0f ? 160.0f : cam_height > 380.0f ? 380.0f : cam_height;
            const float speed = (in->buttons & KILN_BTN_A) ? 640.0f : 220.0f;
            const fm_vec3_t fwd = {{ fm_sinf(yaw), 0, fm_cosf(yaw) }};
            const fm_vec3_t right = {{ -fwd.v[2], 0, fwd.v[0] }};
            focus.v[0] += (fwd.v[0] * in->stick_y + right.v[0] * in->stick_x) * speed * DT;
            focus.v[2] += (fwd.v[2] * in->stick_y + right.v[2] * in->stick_x) * speed * DT;
        }
        for (int a = 0; a < 3; a += 2) {
            if (focus.v[a] < 60.0f) focus.v[a] = 60.0f;
            if (focus.v[a] > WORLD_SIZE - 60.0f) focus.v[a] = WORLD_SIZE - 60.0f;
        }
        ground += (ground_at(focus.v[0], focus.v[2]) - ground) * (3.0f * DT);

        const fm_vec3_t fwd = {{ fm_sinf(yaw), 0, fm_cosf(yaw) }};
        g_scene.cam_pos = (fm_vec3_t){{ focus.v[0] - fwd.v[0] * 250.0f, ground + cam_height,
                                        focus.v[2] - fwd.v[2] * 250.0f }};
        g_scene.cam_target = (fm_vec3_t){{ focus.v[0] + fwd.v[0] * 40.0f, ground,
                                           focus.v[2] + fwd.v[2] * 40.0f }};

        /* Residency (cheap requests only), then the pacer's real loads. */
        g_bind.camera_pos = g_scene.cam_pos;
        kiln_tile_flush_unload(&g_tiles);
        kiln_tile_update(&g_tiles, focus, lod_from_camera);
        kiln_streamio_pump(&g_io);

        const uint32_t bytes = kiln_stream_bytes_admitted_this_frame(&g_io.stream);
        if (bytes >= bytes_peak || ++bytes_hold > 45) { bytes_peak = bytes; bytes_hold = 0; }

        kiln_scratch_begin(&g_scratch);
        kiln_scene_update(&g_scene);

        /* ── 3D ──────────────────────────────────────────────────────── */
        kiln_frame_begin();
        kiln_scene_begin(&g_scene);

        /* The sea: follows the camera on its own cell grid so the checker
         * never swims, and sits under every tile top. */
        sea_xf.pos = (fm_vec3_t){{ 80.0f * (float)(int)(g_scene.cam_pos.v[0] / 80.0f), -10.0f,
                                   80.0f * (float)(int)(g_scene.cam_pos.v[2] / 80.0f) }};
        kiln_transform_push(&sea_xf);
        kiln_prim_draw(&g_sea);
        kiln_transform_pop();

        kiln_twopass_render(&g_tiles.visual, &g_lod, &g_scratch, &g_scene, pass_draw, NULL);

        /* ── 2D ──────────────────────────────────────────────────────── */
        kiln_gui_begin();

        uint16_t loaded = 0, waiting = 0, per_lod[3] = { 0, 0, 0 };
        for (KilnTileSlot *s = kiln_tile_first(&g_tiles.visual); s;
             s = kiln_tile_next(&g_tiles.visual, s)) {
            if (!tile_live(s)) continue;
            loaded++;
            if (!s->user_data) waiting++;
            if (s->lod < 3) per_lod[s->lod]++;
        }

        if (overlay) {
            kiln_dd_begin(&g_scene, SCREEN_W, SCREEN_H);
            for (KilnTileSlot *s = kiln_tile_first(&g_tiles.visual); s;
                 s = kiln_tile_next(&g_tiles.visual, s)) {
                if (!tile_live(s)) continue;
                const float x0 = s->world_x * TILE_SIZE, z0 = s->world_y * TILE_SIZE;
                const float h = tile_height(s->world_x, s->world_y) + 2.0f;
                kiln_dd_aabb((fm_vec3_t){{ x0 + 3, h, z0 + 3 }},
                             (fm_vec3_t){{ x0 + TILE_SIZE - 3, h, z0 + TILE_SIZE - 3 }},
                             s->user_data ? LOD_COL[s->lod < 3 ? s->lod : 2] : RED);
            }
            kiln_dd_end();
        }

        /* Top left: the world. */
        kiln_gui_panel(8, 8, 150, 58, PANEL, TEAL);
        kiln_gui_text(14, 21, TEAL, "KILN OPEN WORLD");
        kiln_gui_text(118, 21, DIM, "%2d", (int)(fps + 0.5f));
        kiln_gui_text(14, 34, INK, "tiles %2u/49 cache %2u", loaded, kiln_cache_count(&g_cache));
        kiln_gui_text(14, 46, INK, "L0 %2u  L1 %2u  L2 %2u", per_lod[0], per_lod[1], per_lod[2]);
        kiln_gui_text(14, 58, DIM, "scratch %2u%%",
                      (unsigned)(g_scratch.offset * 100 / KILN_SCRATCH_SIZE));

        /* Top right: the pacer. Every number here is 0 when healthy, and
         * goes red at the value that means the stream is behind or broken. */
        const unsigned pending = kiln_stream_pending_count(&g_io.stream);
        const unsigned long dropped = (unsigned long)kiln_stream_dropped_total(&g_io.stream);
        const unsigned long failed = (unsigned long)kiln_streamio_fail_total(&g_io);
        const int px = SCREEN_W - 8 - 144;
        kiln_gui_panel(px, 8, 144, 70, PANEL, TEAL);
        kiln_gui_text(px + 6, 21, TEAL, "STREAM PACER");
        kiln_gui_text(px + 6, 34, pending > PENDING_RED ? RED : INK, "pend %2u/%d", pending,
                      KILN_STREAM_MAX_PENDING);
        kiln_gui_text(px + 84, 34, DIM, "hi %2u", (unsigned)kiln_stream_high_water(&g_io.stream));
        kiln_gui_text(px + 6, 46, dropped ? RED : INK, "drop %lu", dropped);
        kiln_gui_text(px + 72, 46, failed ? RED : INK, "fail %lu", failed);
        kiln_gui_text(px + 6, 58, INK, "kb/f %u.%u of %u", (unsigned)(bytes_peak / 1024),
                      (unsigned)((bytes_peak % 1024) * 10 / 1024), (unsigned)(STREAM_BYTES / 1024));
        kiln_gui_bar(px + 6, 64, 132, 5, (float)bytes_peak / (float)STREAM_BYTES,
                     bytes_peak >= STREAM_BYTES - 1024 ? AMBER : TEAL, PANEL);

        /* Bottom right: the island, and what is resident right now. */
        kiln_gui_panel(MAP_X, MAP_Y, WORLD_N * MAP_PX + 2, WORLD_N * MAP_PX + 2, PANEL, TEAL);
        for (int i = 0; i < g_run_count; i++) {
            const MapRun *r = &g_runs[i];
            kiln_gui_rect(MAP_X + 1 + r->x * MAP_PX, MAP_Y + 1 + r->y * MAP_PX,
                          r->w * MAP_PX, MAP_PX, BIOME_MAP[r->biome]);
        }
        for (KilnTileSlot *s = kiln_tile_first(&g_tiles.visual); s;
             s = kiln_tile_next(&g_tiles.visual, s)) {
            if (!tile_live(s)) continue;
            kiln_gui_rect(map_x(s->world_x), map_y(s->world_y), MAP_PX, MAP_PX,
                          s->user_data ? LOD_COL[s->lod < 3 ? s->lod : 2] : RED);
        }
        {
            const int fx = map_x((int)(focus.v[0] / TILE_SIZE));
            const int fy = map_y((int)(focus.v[2] / TILE_SIZE));
            kiln_gui_line(fx + 1, fy + 1, fx + 1 - (int)(fwd.v[0] * 8), fy + 1 - (int)(fwd.v[2] * 8),
                          1, RGBA32(0x10, 0x10, 0x10, 0xFF));
        }

        if (autopilot) {
            kiln_gui_panel(8, SCREEN_H - 44, KILN_JUMP == JUMP_FAST ? 96 : 72, 16,
                           RGBA32(0xC0, 0x30, 0x60, 0xFF), INK);
            kiln_gui_text(14, SCREEN_H - 32, INK, KILN_JUMP == JUMP_FAST ? "AUTOPILOT x3.5" : "AUTOPILOT");
        }
        if (waiting) kiln_gui_text(14, 84, AMBER, "%u proxies", waiting);

        kiln_gui_panel(8, SCREEN_H - 24, MAP_X - 16, 16, PANEL, DIM);
        kiln_gui_text(14, SCREEN_H - 12, INK, "stick move  A run  C orbit  Z LODs");

        kiln_gui_end();
        kiln_frame_end();
    }
}
