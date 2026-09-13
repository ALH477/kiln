// SPDX-License-Identifier: MIT
//
// assets-demo: the asset pipeline, end to end. Six converted assets ride in one
// StreamDB pak and a seventh stays loose on DragonFS, and every one of them is
// on screen:
//
//   models/{interceptor,torus,sphere,cone,cube}.t3dm
//                       -> kiln_asset_model, loaded out of the pak on demand
//                          and shown on a lit turntable
//   sprites/logo.sprite -> kiln_asset_sprite, bouncing in the 2D pass
//   sfx/blip.wav64      -> loose DFS, because wav64_open has no buffer form: a
//                          blip on every bounce, measured back off the mixer's
//                          own output as a VU bar
//
// The HUD reads the pak rather than restating it: kiln_asset_count for how many
// documents it holds, kiln_asset_find_suffix for how many are models, and
// kiln_asset_exists / kiln_asset_size for each key — so a key that fell out of
// the build is a red line, not a blank turntable. (A StreamDB index entry
// carries no key string, so the key NAMES come from the table below; whether
// each one is really in the pak comes from the pak.)
//
// Model scale and height are taken from each model's own bounding box.
//
//   D< D>  previous / next model      A  kick the logo
//   idle 3 s: the demo cycles the models and kicks the logo itself
//
// Jump: .#assets-demo-ship boots on the interceptor and stays there.

#include <libdragon.h>
#include <t3d/t3dmodel.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_prim.h>
#include <kiln/kiln_asset.h>

#include <malloc.h>
#include <string.h>

enum { JUMP_NONE, JUMP_SHIP };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W 320
#define SCREEN_H 240
#define SAMPLE_RATE 32000
#define CH_BLIP 0
#define PAK_PATH "rom:/assets-demo.streamdb"

#define TABLE_TOP 12.0f      /* turntable surface, world units */
#define MODEL_RADIUS 26.0f   /* every model is fitted to this   */

typedef struct { const char *key; const char *file; } PakEntry;

/* Models first, in turntable order; the sprite last. */
static const PakEntry PAK[] = {
    { "models/interceptor.t3dm", "interceptor.t3dm" },
    { "models/torus.t3dm",       "torus.t3dm" },
    { "models/sphere.t3dm",      "sphere.t3dm" },
    { "models/cone.t3dm",        "cone.t3dm" },
    { "models/cube.t3dm",        "cube.t3dm" },
    { "sprites/logo.sprite",     "logo.sprite" },
};
#define PAK_KEYS   ((int)(sizeof(PAK) / sizeof(PAK[0])))
#define PAK_MODELS (PAK_KEYS - 1)

// ── Tapes ───────────────────────────────────────────────────────────────
// Five seconds a model, with two kicks of the logo in between.
static const KilnInputKey ATTRACT_KEYS[] = {
    { .frame =   0 },
    { .frame =  50, .buttons = KILN_BTN_A },
    { .frame =  56 },
    { .frame = 190, .buttons = KILN_BTN_A },
    { .frame = 196 },
    { .frame = 294, .buttons = KILN_BTN_DR },
    { .frame = 300 },
};
static const KilnInputTape ATTRACT = { ATTRACT_KEYS, 7, 0 };

// ── The model on the turntable ──────────────────────────────────────────
static T3DModel *g_model;
static int g_index = -1;
static uint32_t g_load_us;
static float g_fit, g_lift, g_pop;

static int count_cb(const streamdb_emb_doc_t *doc, void *user)
{
    (void)doc;
    (*(int *)user)++;
    return 0;
}

static void show_model(const KilnAsset *db, int idx)
{
    if (g_model) {
        t3d_model_free(g_model);
        g_model = NULL;
    }
    g_index = idx;
    const char *key = PAK[idx].key;
    const uint64_t t0 = get_ticks_us();
    g_model = kiln_asset_model(db, key, strlen(key));
    g_load_us = (uint32_t)(get_ticks_us() - t0);
    if (!g_model) return;   /* the HUD shows it red; nothing to draw */

    /* Fit from the model's own box (gltf_to_t3d writes it from the chunks):
     * the largest extent from the origin maps to MODEL_RADIUS, and the lowest
     * point sits on the turntable. */
    float r = 1.0f;
    for (int a = 0; a < 3; a++) {
        const float lo = -(float)g_model->aabbMin[a], hi = (float)g_model->aabbMax[a];
        if (lo > r) r = lo;
        if (hi > r) r = hi;
    }
    g_fit = MODEL_RADIUS / r;
    g_lift = TABLE_TOP + 1.0f - (float)g_model->aabbMin[1] * g_fit;
    g_pop = 0.0f;
}

// ── The bouncing logo ───────────────────────────────────────────────────
#define BOX_X (SCREEN_W - 112)
#define BOX_Y 8
#define BOX_W 104
#define BOX_H 84
#define LOGO_SCALE 1.5f

typedef struct {
    float x, y, vx, vy;
    float squash;   /* 1 on impact, decays to 0 */
    int bounces;
    int rest;       /* frames spent resting on the floor */
} Logo;

static void logo_kick(Logo *l, int dir)
{
    /* Apex = vy^2 / (2g) = 5.6^2 / 0.64 = 49 px, inside the 84 px box with the
     * 48 px logo. It was 7.5 (88 px) and the logo left the box through the
     * top in Ares; the ceiling below now catches any kick that still would. */
    l->vy = -5.6f;
    l->vx = dir > 0 ? 2.6f : -2.6f;
    l->rest = 0;
}

static void logo_step(Logo *l, float size, wav64_t *blip)
{
    const float floor_y = (float)(BOX_Y + BOX_H) - 3.0f - size;
    const float left = (float)BOX_X + 3.0f, right = (float)(BOX_X + BOX_W) - 3.0f - size;

    l->vy += 0.32f;
    l->x += l->vx;
    l->y += l->vy;
    if (l->squash > 0.0f) l->squash -= 0.08f;

    if (l->y >= floor_y) {
        l->y = floor_y;
        const float impact = l->vy;
        if (impact > 1.2f) {
            l->vy = -impact * 0.78f;
            l->squash = impact > 6.0f ? 1.0f : impact / 6.0f;
            l->bounces++;
            const float vol = impact > 6.0f ? 1.0f : impact / 6.0f;
            mixer_ch_set_vol(CH_BLIP, vol, vol);
            wav64_play(blip, CH_BLIP);
        } else {
            l->vy = 0.0f;
            l->vx *= 0.9f;
            l->rest++;
        }
    }
    const float top = (float)BOX_Y + 3.0f;
    if (l->y < top) { l->y = top; if (l->vy < 0.0f) l->vy = -l->vy * 0.5f; }
    if (l->x < left)  { l->x = left;  l->vx = -l->vx; }
    if (l->x > right) { l->x = right; l->vx = -l->vx; }
}

int main(void)
{
    debug_init_isviewer();
    kiln_engine_init(RESOLUTION_320x240);
    dfs_init(DFS_DEFAULT_LOCATION);
    joypad_init();
    kiln_input_init();

    audio_init(SAMPLE_RATE, 4);
    mixer_init(1);
    wav64_t blip;
    wav64_open(&blip, "rom:/sfx/blip.wav64");

    const size_t need = kiln_asset_probe_size(PAK_PATH);
    assertf(need > 0, "assets-demo: %s not found / probe failed", PAK_PATH);
    void *arena = malloc(need);
    assertf(arena, "assets-demo: arena malloc %zu failed", need);
    KilnAsset *db = kiln_asset_open(PAK_PATH, arena, need);
    assertf(db, "assets-demo: kiln_asset_open failed");

    /* What the pak itself says it holds. */
    const uint32_t doc_count = kiln_asset_count(db);
    int t3dm_count = 0;
    kiln_asset_find_suffix(db, ".t3dm", 5, count_cb, &t3dm_count);
    int present[PAK_KEYS];
    uint32_t bytes[PAK_KEYS];
    for (int i = 0; i < PAK_KEYS; i++) {
        const size_t n = strlen(PAK[i].key);
        present[i] = kiln_asset_exists(db, PAK[i].key, n);
        bytes[i] = (uint32_t)kiln_asset_size(db, PAK[i].key, n);
    }

    sprite_t *logo = kiln_asset_sprite(db, PAK[PAK_MODELS].key, strlen(PAK[PAK_MODELS].key));
    show_model(db, 0);

    // ── stage ───────────────────────────────────────────────────────────
    KilnScene scene;
    kiln_scene_init(&scene);
    kiln_prim_stage(&scene, RGBA32(0x58, 0x64, 0x88, 0xFF), 260.0f, 520.0f);
    scene.fov_deg = 58.0f;
    scene.near_z = 6.0f;
    scene.far_z = 600.0f;

    KilnPrim floor, plinth, table;
    kiln_prim_floor(&floor, 260.0f, 12, kiln_prim_rgba(0x8C, 0x92, 0xA8), kiln_prim_rgba(0x6C, 0x72, 0x88));
    kiln_prim_box(&plinth, (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 40, TABLE_TOP / 2 - 2, 40 }},
                  kiln_prim_rgba(0x50, 0x54, 0x64), kiln_prim_rgba(0x3C, 0x40, 0x4C),
                  kiln_prim_rgba(0x20, 0x20, 0x28));
    kiln_prim_box(&table, (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 46, 2, 46 }},
                  kiln_prim_rgba(0xE8, 0xA0, 0x48), kiln_prim_rgba(0xB0, 0x70, 0x30),
                  kiln_prim_rgba(0x60, 0x40, 0x20));

    KilnTransform xf_floor, xf_plinth, xf_table, xf_model;
    kiln_transform_init(&xf_floor);
    kiln_transform_init(&xf_plinth);
    kiln_transform_init(&xf_table);
    kiln_transform_init(&xf_model);
    xf_plinth.pos = (fm_vec3_t){{ 0, TABLE_TOP / 2 - 2, 0 }};
    xf_table.pos = (fm_vec3_t){{ 0, TABLE_TOP - 2, 0 }};
    xf_table.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
    xf_model.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};

    if (KILN_JUMP == JUMP_NONE) kiln_input_set_attract(1, &ATTRACT, 180);

    const float logo_size = (logo ? logo->width : 32) * LOGO_SCALE;
    Logo lg = { .x = BOX_X + 20, .y = BOX_Y + 4, .vx = 1.8f };
    float spin = 0.0f, t = 0.0f, vu = 0.0f;
    int kick_dir = 1;

    for (;;) {
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);
        const float dt = 1.0f / 60.0f;
        t += dt;

        if (KILN_JUMP == JUMP_NONE) {
            if (in->edges & KILN_BTN_DR) show_model(db, (g_index + 1) % PAK_MODELS);
            if (in->edges & KILN_BTN_DL) show_model(db, (g_index + PAK_MODELS - 1) % PAK_MODELS);
        }
        if (in->edges & KILN_BTN_A) { logo_kick(&lg, kick_dir); kick_dir = -kick_dir; }

        /* Left alone, the logo is kicked again after a second at rest. */
        if (lg.rest > 60) { logo_kick(&lg, kick_dir); kick_dir = -kick_dir; }
        logo_step(&lg, logo_size, &blip);

        spin += 0.9f * dt;
        if (g_pop < 1.0f) g_pop += 4.0f * dt;
        const float pop = g_pop >= 1.0f ? 1.0f : 0.6f + 0.4f * g_pop;
        xf_table.rot_angle = spin;
        xf_model.rot_angle = spin;
        xf_model.scale = (fm_vec3_t){{ g_fit * pop, g_fit * pop, g_fit * pop }};
        xf_model.pos = (fm_vec3_t){{ 0, g_lift, 0 }};

        /* Aimed to the model's +X and above it, so the turntable stands right
         * of centre (screen-right is -X) and low — the one part of the frame
         * clear of both the key list and the logo box. A 36-unit sphere at
         * 132 units filled half the frame and hid behind the key list. */
        scene.cam_target = (fm_vec3_t){{ 44, TABLE_TOP + MODEL_RADIUS + 30, 0 }};
        scene.cam_pos = (fm_vec3_t){{ 52 + 10 * fm_sinf(t * 0.35f), 100, -170 }};
        kiln_scene_update(&scene);

        // ── 3D ──────────────────────────────────────────────────────────
        kiln_frame_begin();
        kiln_scene_begin(&scene);

        kiln_transform_push(&xf_floor);  kiln_prim_draw(&floor);  kiln_transform_pop();
        kiln_transform_push(&xf_plinth); kiln_prim_draw(&plinth); kiln_transform_pop();
        kiln_transform_push(&xf_table);  kiln_prim_draw(&table);  kiln_transform_pop();
        if (g_model) {
            kiln_transform_push(&xf_model);
            t3d_model_draw(g_model);
            kiln_transform_pop();
        }

        // ── 2D ──────────────────────────────────────────────────────────
        kiln_gui_begin();
        const color_t ink = RGBA32(0xE8, 0xE8, 0xF0, 0xFF);
        const color_t teal = RGBA32(0x00, 0xF5, 0xD4, 0xFF);
        const color_t dim = RGBA32(0x98, 0xA0, 0xB8, 0xFF);
        const color_t red = RGBA32(0xFF, 0x50, 0x50, 0xFF);
        const color_t navy = RGBA32(0x0C, 0x10, 0x1C, 0xFF);

        kiln_gui_panel(BOX_X, BOX_Y, BOX_W, BOX_H, navy, RGBA32(0x8B, 0x5C, 0xF6, 0xFF));
        if (logo) {
            const float sq = lg.squash > 0.0f ? lg.squash * 0.3f : 0.0f;
            const float sx = LOGO_SCALE * (1.0f + sq), sy = LOGO_SCALE * (1.0f - sq);
            const float w = logo->width * sx, h = logo->height * sy;
            rdpq_set_mode_standard();
            rdpq_sprite_blit(logo, lg.x + (logo_size - w) / 2, lg.y + (logo_size - h),
                             &(rdpq_blitparms_t){ .scale_x = sx, .scale_y = sy });
        } else {
            kiln_gui_text(BOX_X + 8, BOX_Y + 44, red, "logo MISSING");
        }
        /* VU: the peak of what the mixer actually wrote, decaying. */
        const color_t vu_c = vu > 0.8f ? red : (vu > 0.45f ? RGBA32(0xFF, 0xD0, 0x40, 0xFF) : teal);
        kiln_gui_panel(BOX_X, BOX_Y + BOX_H + 4, BOX_W, 30, navy, RGBA32(0x8B, 0x5C, 0xF6, 0xFF));
        kiln_gui_text(BOX_X + 6, BOX_Y + BOX_H + 16, dim, "VU");
        kiln_gui_bar(BOX_X + 22, BOX_Y + BOX_H + 9, BOX_W - 28, 7, vu, vu_c, RGBA32(0x30, 0x34, 0x44, 0xFF));
        kiln_gui_text(BOX_X + 6, BOX_Y + BOX_H + 29, dim, "bounce %d", lg.bounces);

        kiln_gui_panel(8, 8, 176, 124, navy, teal);
        kiln_gui_text(14, 21, teal, "KILN ASSETS  streamdb pak");
        kiln_gui_text(14, 33, ink, "docs %lu  .t3dm %d", (unsigned long)doc_count, t3dm_count);
        for (int i = 0; i < PAK_KEYS; i++) {
            const int y = 47 + i * 11;
            const int cur = i == g_index;
            const color_t c = !present[i] ? red : (cur ? teal : dim);
            kiln_gui_text(14, y, c, "%c %-17s%5.1fK", cur ? '>' : ' ', PAK[i].file,
                          (float)bytes[i] / 1024.0f);
        }
        if (g_model) {
            kiln_gui_text(14, 114, ink, "verts %u  load %.1f ms",
                          g_model->totalVertCount, (float)g_load_us / 1000.0f);
        } else {
            kiln_gui_text(14, 114, red, "%s did not load", PAK[g_index].file);
        }
        kiln_gui_text(14, 126, dim, "sfx/blip.wav64 loose DFS");

        if (kiln_input_scripted(1)) {
            kiln_gui_panel(SCREEN_W - 58, SCREEN_H - 44, 50, 16, RGBA32(0xC0, 0x30, 0x60, 0xFF),
                           RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
            kiln_gui_text(SCREEN_W - 49, SCREEN_H - 32, RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "DEMO");
        }
        kiln_gui_panel(8, SCREEN_H - 24, SCREEN_W - 16, 16, navy, RGBA32(0x8B, 0x5C, 0xF6, 0xFF));
        kiln_gui_text(14, SCREEN_H - 12, ink, "D< D> model    A kick the logo");

        kiln_gui_end();
        kiln_frame_end();

        float peak = 0.0f;
        while (audio_can_write()) {
            short *buf = audio_write_begin();
            const int n = audio_get_buffer_length();
            mixer_poll(buf, n);
            for (int i = 0; i < n * 2; i++) {
                const float s = buf[i] < 0 ? -(float)buf[i] : (float)buf[i];
                if (s > peak) peak = s;
            }
            audio_write_end();
        }
        peak /= 32768.0f;
        vu = peak > vu ? peak : vu * 0.9f;
    }
}
