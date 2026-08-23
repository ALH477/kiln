// SPDX-License-Identifier: MIT
//
// Phase C verification: same three asset kinds as examples/assets-demo
// (model, sprite, raw data), but loaded from a single StreamDB container
// mounted at boot instead of loose DFS files. If kiln_asset_open,
// kiln_asset_model, kiln_asset_sprite, kiln_asset_load, kiln_asset_count, and
// kiln_asset_find_suffix all work, this ROM looks identical to assets-demo
// on screen — the HUD shows the DB's doc count and a suffix-search result
// where assets-demo shows nothing of the kind.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_asset.h>

#include <malloc.h>

#define SCREEN_W 320
#define SCREEN_H 240

// A level-layout blob, the kind of thing StreamDB is actually for: not an
// engine-loaded asset (model/sprite/wav64) but game data the title code
// reads by key. Here it's a tiny header + four vec3 player-spawn points,
// just enough that the ROM can prove it read the bytes the build packed.
static const char *LEVEL_KEY = "levels/intro.bin";

static int g_suffix_t3dm;
static int count_t3dm_cb(const streamdb_emb_doc_t *d, void *user)
{
    (void)d; (void)user;
    g_suffix_t3dm++;
    return 0;
}

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);

    // Size the arena at boot, not by guessing. kiln_asset_probe_size reads
    // the DB header over DFS and reports what streamdb_emb_open will need.
    size_t need = kiln_asset_probe_size("rom:/assets.streamdb");
    if (need == 0) {
        // No DB is a content fact (wrong ROM), not a programming error — but
        // there is nothing to demo without it, so assert and stop.
        assertf(false, "streamdb-demo: assets.streamdb not found / probe failed");
    }
    void *arena = malloc(need);
    assertf(arena, "streamdb-demo: arena malloc %zu failed", need);

    KilnAsset *db = kiln_asset_open("rom:/assets.streamdb", arena, need);
    assertf(db, "streamdb-demo: kiln_asset_open failed");

    // Level-layout blob: read raw bytes (no in-memory parser for these —
    // the game interprets them itself). kiln_asset_size lets the buffer be
    // sized exactly; kiln_asset_load CRC-verifies the payload.
    size_t level_len = kiln_asset_size(db, LEVEL_KEY, strlen(LEVEL_KEY));
    assertf(level_len > 0, "streamdb-demo: '%s' not in DB", LEVEL_KEY);
    uint8_t *level = malloc(level_len);
    size_t got = level_len;
    int r = kiln_asset_load(db, LEVEL_KEY, strlen(LEVEL_KEY), level, &got);
    assertf(r == STREAMDB_EMB_OK, "streamdb-demo: level load: %s",
            streamdb_emb_strerror(r));

    // Sanity: the build packs a 4-byte magic 'KLNL' at the head of the blob.
    assertf(level_len >= 4 && memcmp(level, "KLNL", 4) == 0,
            "streamdb-demo: level blob has bad magic");

    // Models and sprites, parsed in place from the StreamDB payload. These
    // two paths are the whole point of the Tiny3D patch and the
    // sprite_load_buf wrap respectively.
    T3DModel *model = kiln_asset_model(db, "models/cube.t3dm", 16);
    assertf(model, "streamdb-demo: kiln_asset_model failed");

    sprite_t *logo = kiln_asset_sprite(db, "sprites/logo.sprite", 19);
    assertf(logo, "streamdb-demo: kiln_asset_sprite failed");

    // Suffix search: every key in the DB ending in ".t3dm". This is the
    // reverse-trie O(suffix + matches) scan, not a full table walk.
    kiln_asset_find_suffix(db, ".t3dm", 5, count_t3dm_cb, NULL);

    uint32_t doc_count = kiln_asset_count(db);

    KilnScene scene;
    kiln_scene_init(&scene);
    scene.cam_pos = (fm_vec3_t){{ 0, 14, -70 }};
    scene.far_z = 300.0f;

    KilnTransform xform;
    kiln_transform_init(&xform);
    xform.scale = (fm_vec3_t){{ 1.0f, 1.0f, 1.0f }};

    float spin = 0.0f;
    uint32_t frames = 0;
    float fps = 0.0f;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        joypad_poll();

        spin += 0.02f;
        kiln_scene_update(&scene);

        xform.rot_angle = spin;
        xform.rot_axis = (fm_vec3_t){{ 0.3f, 1.0f, 0.15f }};
        fm_vec3_norm(&xform.rot_axis, &xform.rot_axis);

        kiln_frame_begin();
        kiln_scene_begin(&scene);

        kiln_transform_push(&xform);
        t3d_model_draw(model);
        kiln_transform_pop();

        kiln_gui_begin();

        rdpq_sprite_upload(TILE0, logo, NULL);
        rdpq_texture_rectangle(TILE0, SCREEN_W - 40, 8, SCREEN_W - 8, 40, 0, 0);

        kiln_gui_panel(8, 8, 180, 82,
                      RGBA32(10, 10, 24, 200), RGBA32(0, 245, 212, 255));
        kiln_gui_text(14, 22, RGBA32(0, 245, 212, 255), "KILN STREAMDB");
        kiln_gui_text(14, 34, RGBA32(232, 232, 240, 255), "fps %5.1f", fps);
        kiln_gui_text(14, 46, RGBA32(232, 232, 240, 255), "docs %lu",
                     (unsigned long)doc_count);
        kiln_gui_text(14, 58, RGBA32(232, 232, 240, 255), "suffix .t3dm -> %d", g_suffix_t3dm);
        kiln_gui_text(14, 70, RGBA32(232, 232, 240, 255), "level %zuB magic %.4s",
                     level_len, level);
        kiln_gui_end();

        kiln_frame_end();

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }
    }
}