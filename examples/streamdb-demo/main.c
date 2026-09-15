// SPDX-License-Identifier: MIT
//
// One StreamDB container, everything on screen came out of it.
//
// rom:/assets.streamdb packs three documents (flake.nix `demoStreamdb`):
//
//   models/cube.t3dm      kiln_asset_model   -> t3d_model_load_buf
//   sprites/logo.sprite   kiln_asset_sprite  -> sprite_load_buf
//   levels/intro.bin      kiln_asset_load    -> raw bytes the game parses
//
// The level blob is 'KLNL', a u32 spawn count and that many vec3 spawn points,
// LITTLE-endian (python's struct "<"). The console is big-endian, so it is
// decoded byte by byte here — a memcpy onto a struct reads a count of 4 as
// 67,108,864. Each spawn gets a pedestal and a copy of the DB's cube, with
// kiln_debugdraw axes at the spawn point and a label naming where it came from.
//
// The HUD lists every key through kiln_asset_find_suffix. A StreamDB document
// record carries no key (UUID, offset, size, CRC), so the scan is by suffix —
// one per kind of document — and the panel goes red unless the hits add up to
// kiln_asset_count, i.e. unless every document is accounted for, and each
// hit's size agrees with kiln_asset_size on the key it should be.
//
//   stick  orbit + raise      A  fly to the next spawn, then back out
//   idle 3 s: the demo tours the spawns itself
//
// Jump ROM: .#streamdb-demo-focus holds the camera on spawn 3, the one the
// layout puts in the air.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_asset.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_prim.h>
#include <kiln/kiln_debugdraw.h>

#include <malloc.h>
#include <string.h>

enum { JUMP_NONE, JUMP_FOCUS };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W 320
#define SCREEN_H 240
#define DB_PATH "rom:/assets.streamdb"
#define MAX_SPAWNS 8
/* Blob units to world units: the blob's spawns are 10 apart, the cube is 14
 * across at the scale it is drawn, and spawn 3 sits 5 units above spawn 0 —
 * 40 world units, enough air between its pedestal and cube 0's top. */
#define LAYOUT_SCALE 8.0f
#define CUBE_SCALE 0.5f     /* baseScale 28 -> a 28-unit cube -> 14 */
#define PEDESTAL_H 6.0f

static const color_t INK  = { 232, 232, 240, 255 };
static const color_t HEAD = { 0, 245, 212, 255 };
static const color_t OK   = { 96, 230, 128, 255 };
static const color_t BAD  = { 255, 90, 90, 255 };
static const color_t DIM  = { 144, 152, 176, 255 };
static const color_t FILL = { 10, 10, 24, 255 };

// ── the key table ──────────────────────────────────────────────────────
typedef struct {
    const char *key;
    const char *suffix;
    int         hits;
    uint32_t    size;       /* from the suffix scan's document record */
    uint32_t    crc;
    int         agrees;     /* one hit, and its size matches kiln_asset_size */
} KeyRow;

static KeyRow g_rows[] = {
    { .key = "models/cube.t3dm",    .suffix = ".t3dm"   },
    { .key = "sprites/logo.sprite", .suffix = ".sprite" },
    { .key = "levels/intro.bin",    .suffix = ".bin"    },
};
#define NROWS ((int)(sizeof g_rows / sizeof g_rows[0]))

static int suffix_cb(const streamdb_emb_doc_t *d, void *user)
{
    KeyRow *r = user;
    r->hits++;
    r->size = d->size;
    r->crc = d->crc;
    return 0;
}

// ── the level blob ─────────────────────────────────────────────────────
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static float lef32(const uint8_t *p)
{
    uint32_t u = le32(p);
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static const uint32_t PEDESTAL_TOP[4] = { 0x00E0C8FF, 0xA78BFAFF, 0xFFC04CFF, 0xFF6F8CFF };

// ── tape: a slow orbit, visiting each spawn in turn ────────────────────
static const KilnInputKey TOUR_KEYS[] = {
    { .frame = 0,   .sx = 36 },
    { .frame = 140, .buttons = KILN_BTN_A },
    { .frame = 146 },
    { .frame = 320, .buttons = KILN_BTN_A },
    { .frame = 326, .sx = -30 },
    { .frame = 500, .buttons = KILN_BTN_A },
    { .frame = 506 },
    { .frame = 680, .buttons = KILN_BTN_A },
    { .frame = 686, .sx = 30, .sy = 24 },
    { .frame = 860, .buttons = KILN_BTN_A },
    { .frame = 866, .sy = -24 },
    { .frame = 960 },
};
static const KilnInputTape TOUR = { TOUR_KEYS, 12, 0 };

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    kiln_input_init();
    dfs_init(DFS_DEFAULT_LOCATION);

    // ── open the container ─────────────────────────────────────────────
    // kiln_asset_probe_size says what the reader needs; the arena is that,
    // rounded up to a whole KB, so the gauge has headroom to show.
    size_t need = kiln_asset_probe_size(DB_PATH);
    assertf(need > 0, "streamdb-demo: %s not found or not a StreamDB", DB_PATH);
    size_t arena_size = (need + 1023) & ~(size_t)1023;
    void *arena = malloc(arena_size);
    assertf(arena, "streamdb-demo: arena malloc %zu failed", arena_size);
    KilnAsset *db = kiln_asset_open(DB_PATH, arena, arena_size);
    assertf(db, "streamdb-demo: kiln_asset_open failed");

    uint32_t docs = kiln_asset_count(db);
    size_t arena_used = kiln_asset_arena_used(db);

    int scanned = 0, all_ok = 1;
    for (int i = 0; i < NROWS; i++) {
        KeyRow *r = &g_rows[i];
        kiln_asset_find_suffix(db, r->suffix, strlen(r->suffix), suffix_cb, r);
        r->agrees = r->hits == 1 && r->size == kiln_asset_size(db, r->key, strlen(r->key));
        scanned += r->hits;
        all_ok &= r->agrees;
    }
    all_ok &= (uint32_t)scanned == docs;

    // ── the three documents ────────────────────────────────────────────
    const char *level_key = g_rows[2].key;
    size_t level_len = kiln_asset_size(db, level_key, strlen(level_key));
    assertf(level_len >= 8, "streamdb-demo: '%s' missing or short (%zu B)", level_key, level_len);
    uint8_t *level = malloc(level_len);
    size_t got = level_len;
    int rc = kiln_asset_load(db, level_key, strlen(level_key), level, &got);
    assertf(rc == STREAMDB_EMB_OK, "streamdb-demo: level load: %s", streamdb_emb_strerror(rc));
    assertf(memcmp(level, "KLNL", 4) == 0, "streamdb-demo: level blob has bad magic");

    uint32_t nspawn = le32(level + 4);
    assertf(nspawn >= 1 && nspawn <= MAX_SPAWNS && level_len >= 8 + 12 * (size_t)nspawn,
            "streamdb-demo: level blob claims %lu spawns in %zu bytes",
            (unsigned long)nspawn, level_len);
    fm_vec3_t spawn[MAX_SPAWNS], world[MAX_SPAWNS];
    for (uint32_t i = 0; i < nspawn; i++) {
        const uint8_t *p = level + 8 + 12 * i;
        spawn[i] = (fm_vec3_t){{ lef32(p), lef32(p + 4), lef32(p + 8) }};
        fm_vec3_scale(&world[i], &spawn[i], LAYOUT_SCALE);
    }

    T3DModel *cube = kiln_asset_model(db, g_rows[0].key, strlen(g_rows[0].key));
    assertf(cube, "streamdb-demo: kiln_asset_model failed");
    sprite_t *logo = kiln_asset_sprite(db, g_rows[1].key, strlen(g_rows[1].key));
    assertf(logo, "streamdb-demo: kiln_asset_sprite failed");

    // ── the stage ──────────────────────────────────────────────────────
    KilnScene scene;
    kiln_scene_init(&scene);
    kiln_prim_stage(&scene, RGBA32(0x44, 0x52, 0x70, 0xFF), 180.0f, 420.0f);
    scene.fov_deg = 60.0f;
    scene.near_z = 6.0f;
    scene.far_z = 440.0f;

    KilnPrim floor_prim, pedestal[4];
    kiln_prim_floor(&floor_prim, 150.0f, 15,
                    kiln_prim_rgba(0x74, 0x7E, 0x94), kiln_prim_rgba(0x64, 0x6E, 0x84));
    for (int i = 0; i < 4; i++)
        kiln_prim_box(&pedestal[i], (fm_vec3_t){{ 0, PEDESTAL_H / 2, 0 }},
                      (fm_vec3_t){{ 12, PEDESTAL_H / 2, 12 }},
                      PEDESTAL_TOP[i], kiln_prim_shade(PEDESTAL_TOP[i], 0.7f),
                      kiln_prim_shade(PEDESTAL_TOP[i], 0.4f));

    KilnTransform floor_xf, ped_xf[MAX_SPAWNS], cube_xf[MAX_SPAWNS];
    kiln_transform_init(&floor_xf);
    for (uint32_t i = 0; i < nspawn; i++) {
        kiln_transform_init(&ped_xf[i]);
        kiln_transform_init(&cube_xf[i]);
        ped_xf[i].pos = world[i];
        cube_xf[i].pos = world[i];
        cube_xf[i].pos.v[1] += PEDESTAL_H + 14.0f * CUBE_SCALE + 2.0f;
        cube_xf[i].scale = (fm_vec3_t){{ CUBE_SCALE, CUBE_SCALE, CUBE_SCALE }};
        cube_xf[i].rot_axis = (fm_vec3_t){{ 0.25f, 1.0f, 0.1f }};
        fm_vec3_norm(&cube_xf[i].rot_axis, &cube_xf[i].rot_axis);
    }

    // The overview looks at the layout's centre; focus index == nspawn is it.
    fm_vec3_t centre = {{ 0 }};
    for (uint32_t i = 0; i < nspawn; i++) fm_vec3_add(&centre, &centre, &world[i]);
    fm_vec3_scale(&centre, &centre, 1.0f / (float)nspawn);
    centre.v[1] = 14.0f;

    uint32_t focus = nspawn;
    float orbit = 0.35f, height = 70.0f, drift = 0.12f;
    if (KILN_JUMP == JUMP_FOCUS) {
        focus = nspawn - 1;
        orbit = 0.55f;
        drift = 0.0f;
    } else {
        kiln_input_set_attract(1, &TOUR, 180);
    }
    fm_vec3_t look = centre;
    float radius = 150.0f;
    float spin = 0.0f;

    for (;;) {
        // ── update ──────────────────────────────────────────────────
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);
        const float dt = 1.0f / 60.0f;

        if (KILN_JUMP == JUMP_NONE && kiln_input_pressed(1, KILN_BTN_A))
            focus = (focus + 1) % (nspawn + 1);
        orbit += (in->stick_x * 1.4f + drift) * dt;
        height += in->stick_y * 60.0f * dt;
        if (height < 16.0f) height = 16.0f;
        if (height > 150.0f) height = 150.0f;

        fm_vec3_t want = centre;
        float want_r = 150.0f;
        if (focus < nspawn) {
            want = cube_xf[focus].pos;
            want_r = 85.0f;
        }
        const float k = 3.0f * dt;
        fm_vec3_lerp(&look, &look, &want, k);
        radius += (want_r - radius) * k;

        const float h = focus < nspawn ? height * 0.45f : height;
        scene.cam_target = look;
        scene.cam_pos = (fm_vec3_t){{ look.v[0] + fm_sinf(orbit) * radius,
                                      look.v[1] + h,
                                      look.v[2] - fm_cosf(orbit) * radius }};
        kiln_scene_update(&scene);

        spin += dt;
        for (uint32_t i = 0; i < nspawn; i++)
            cube_xf[i].rot_angle = spin * (1.0f + 0.3f * (float)i) + (float)i;

        // ── 3D pass ─────────────────────────────────────────────────
        kiln_frame_begin();
        kiln_scene_begin(&scene);

        kiln_transform_push(&floor_xf); kiln_prim_draw(&floor_prim); kiln_transform_pop();
        for (uint32_t i = 0; i < nspawn; i++) {
            kiln_transform_push(&ped_xf[i]);
            kiln_prim_draw(&pedestal[i % 4]);
            kiln_transform_pop();
            kiln_transform_push(&cube_xf[i]);
            t3d_model_draw(cube);
            kiln_transform_pop();
        }

        // ── 2D pass ─────────────────────────────────────────────────
        kiln_gui_begin();

        // Spatial overlay first, so the panels sit on top of it.
        kiln_dd_begin(&scene, SCREEN_W, SCREEN_H);
        for (uint32_t i = 0; i < nspawn; i++) {
            fm_vec3_t base = world[i];
            kiln_dd_axes(base, 20.0f);
            if (base.v[1] > 0.5f) {
                fm_vec3_t ground = {{ base.v[0], 0, base.v[2] }};
                kiln_dd_line(ground, base, RGBA32(255, 224, 96, 255));
            }
            fm_vec3_t tag = cube_xf[i].pos;
            tag.v[1] += 13.0f;
            kiln_dd_text(tag, i == focus ? HEAD : INK, "#%lu", (unsigned long)i);
        }
        kiln_dd_end();

        // Every key, found by suffix.
        kiln_gui_panel(4, 4, 196, 64, FILL, all_ok ? HEAD : BAD);
        kiln_gui_text(10, 17, HEAD, "ASSETS.STREAMDB");
        kiln_gui_text(130, 17, all_ok ? OK : BAD, "%d/%lu docs", scanned, (unsigned long)docs);
        for (int i = 0; i < NROWS; i++) {
            const KeyRow *r = &g_rows[i];
            kiln_gui_text(10, 31 + 12 * i, r->agrees ? INK : BAD, "%-19s", r->key);
            kiln_gui_text(130, 31 + 12 * i, r->agrees ? DIM : BAD, "%6luB",
                          (unsigned long)r->size);
        }

        // The logo, drawn from the sprite the DB handed back.
        kiln_gui_panel(276, 26, 40, 40, FILL, HEAD);
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_TEX);
        rdpq_sprite_upload(TILE0, logo, NULL);
        rdpq_texture_rectangle(TILE0, 280, 30, 312, 62, 0, 0);

        // Arena and the blob.
        kiln_gui_panel(4, 188, 196, 48, FILL, HEAD);
        kiln_gui_text(10, 201, INK, "arena %zu/%zuB", arena_used, arena_size);
        kiln_gui_bar(118, 195, 76, 6, (float)arena_used / (float)arena_size,
                     arena_used * 10 > arena_size * 9 ? BAD : HEAD, RGBA32(42, 42, 62, 255));
        kiln_gui_text(10, 214, INK, "intro.bin %.4s %lu spawns %zuB",
                      (const char *)level, (unsigned long)nspawn, level_len);
        kiln_gui_text(10, 227, DIM, focus < nspawn ? "A next  stick orbit" : "A visit spawns");

        if (focus < nspawn) {
            // The spawn as the blob stores it, before LAYOUT_SCALE.
            kiln_gui_panel(206, 200, 110, 36, FILL, HEAD);
            kiln_gui_text(212, 214, HEAD, "spawn %lu of %lu",
                          (unsigned long)focus, (unsigned long)nspawn);
            kiln_gui_text(212, 227, INK, "blob %d %d %d",
                          (int)spawn[focus].v[0], (int)spawn[focus].v[1],
                          (int)spawn[focus].v[2]);
        }

        if (kiln_input_scripted(1)) {
            kiln_gui_rect(276, 70, 40, 14, RGBA32(0xC0, 0x30, 0x50, 0xFF));
            kiln_gui_text(284, 81, RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "DEMO");
        }

        kiln_gui_end();
        kiln_frame_end();
    }
}
