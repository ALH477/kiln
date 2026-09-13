// SPDX-License-Identifier: MIT
//
// texanim-demo: a diorama with one exhibit per texture / vertex effect, each
// driven through the engine module that owns it.
//
//   UV SCROLL  checker floor     kiln_texanim KILN_TEXANIM_SCROLL (tile callback)
//   ENV MAP    chrome ball       kiln_vfx_set(KILN_VFX_SPHERICAL_UV, 32, 32)
//   CEL SHADE  torus             kiln_vfx_set(KILN_VFX_CELSHADE_COLOR) + 8x1 ramp,
//                                then KILN_VFX_OUTLINE for the ink line
//   FLAG       banner on a pole  kiln_deform — a wave per vertex, two buffers
//   MORPH      blob              kiln_morph — four CPU targets, colour per channel,
//                                normals blended between the two heaviest
//   FLIPBOOK   fireplace         kiln_texanim FLIPBOOK — eight generated frames
//   PALETTE    lava pool         kiln_texanim PALETTE — one CI4 surface, sixteen
//                                rotations of its palette
//   OFFSCREEN  monitor wall      kiln_texanim OFFSCREEN — the torus rendered into a
//                                32x32 surface every frame, sampled as a texture
//
// The last three are texture REFERENCE materials (f3d_inject's useRef), which
// Tiny3D does not upload: kiln_texanim's dynTextureCb does, matched by number.
// TMEM is spent per material upload, not summed: none of these is over 2 KB.
//
// The camera flies to the exhibit in focus and the HUD names it and the call
// behind it. D-pad left/right picks an exhibit; A turns its effect off and on,
// so the difference is visible in place. Idle 3 s: an attract tape tours all
// of them, switching each effect off and back on as it passes.
//
// The host cannot draw any of this (tile callbacks, vertex FX and vertex
// placeholders are console-only), so this ROM is verified in Ares.
//
// Jumps: .#texanim-demo-{scroll,envmap,cel,flag,morph,flip,pal,offscr} boot
// focused on one exhibit with its effect on, and stay there.

#include <libdragon.h>
#include <t3d/t3dmodel.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_prim.h>
#include <kiln/kiln_texanim.h>
#include <kiln/kiln_vanim.h>

#include <string.h>

enum { JUMP_NONE, JUMP_SCROLL, JUMP_ENVMAP, JUMP_CEL, JUMP_FLAG, JUMP_MORPH, JUMP_FLIP, JUMP_PAL, JUMP_OFFSCR };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W 320
#define SCREEN_H 240
#define DT (1.0f / 60.0f)

/* The checker texture is 32 texels a side; wrapping the scroll offset at its
 * period keeps the tile translate small forever with no visible jump. */
#define CHECKER_PERIOD 32.0f

typedef enum {
    MODE_TOUR, MODE_SCROLL, MODE_ENVMAP, MODE_CEL, MODE_FLAG, MODE_MORPH,
    MODE_FLIP, MODE_PAL, MODE_OFFSCR, MODE_COUNT
} Mode;

static const char *const MODE_NAME[MODE_COUNT] = {
    "TOUR", "UV SCROLL", "ENV MAP", "CEL SHADE", "FLAG", "MORPH",
    "FLIPBOOK", "PALETTE", "OFFSCREEN",
};
static const char *const MODE_API[MODE_COUNT] = {
    "every effect at once",
    "kiln_texanim SCROLL",
    "kiln_vfx SPHERICAL_UV",
    "kiln_vfx CELSHADE_COLOR",
    "kiln_deform, 2 buffers",
    "kiln_morph, 4 targets",
    "kiln_texanim FLIPBOOK",
    "kiln_texanim PALETTE",
    "kiln_texanim OFFSCREEN",
};

/* Exhibit plinths on a ring, one per quadrant. Camera starts on -Z looking at
 * +Z, where screen-right is -X. */
static const fm_vec3_t SPOT[MODE_COUNT] = {
    {{   0, 0,   0 }},
    {{   0, 0,   0 }},
    {{  64, 0,  56 }},   /* env map: front left  */
    {{ -64, 0,  56 }},   /* cel:     front right */
    {{ -64, 0, -60 }},   /* flag:    back right  */
    {{  64, 0, -60 }},   /* morph:   back left   */
    {{   0, 0, 118 }},   /* flipbook: the fireplace, far side, facing +Z */
    {{   0, 0,-104 }},   /* palette:  the lava pool, near side, flat     */
    {{ 132, 0,   0 }},   /* offscreen: the monitor wall, left, facing +X */
};
#define PLINTH_TOP 24.0f
#define POLE_H     80.0f
#define FLAG_LEN   54.0f   /* world length the banner is fitted to */
#define SUBJECT_Y  (PLINTH_TOP + 30.0f)

// ── Tapes ───────────────────────────────────────────────────────────────
// Six seconds per exhibit: effect off at 2.8 s, back on at 3.8 s, next at 6.
static const KilnInputKey ATTRACT_KEYS[] = {
    { .frame =   0 },
    { .frame = 170, .buttons = KILN_BTN_A },
    { .frame = 176 },
    { .frame = 230, .buttons = KILN_BTN_A },
    { .frame = 236 },
    { .frame = 354, .buttons = KILN_BTN_DR },
    { .frame = 360 },
};
static const KilnInputTape ATTRACT = { ATTRACT_KEYS, 7, 0 };

// ── Assets ──────────────────────────────────────────────────────────────
static T3DModel *g_floor, *g_env, *g_torus, *g_flag, *g_blob;

// ── Floor: kiln_texanim scroll ─────────────────────────────────────────
// 40 and 20 texels a second: the old 10 and 5 moved a third of a tile a
// second, and read as a still floor in any capture.
#define SCROLL_S 40.0f
#define SCROLL_T 20.0f
static KilnTexAnim g_scroll = {
    .mode = KILN_TEXANIM_SCROLL,
    .scroll = { .s_speed = SCROLL_S, .t_speed = SCROLL_T },
};

// ── The three reference-material exhibits ──────────────────────────────
// Each number is the model's f3d refAddress (flake.nix), which is how
// kiln_texanim's dynTextureCb knows which material to upload into.
#define REF_FIRE    1
#define REF_LAVA    2
#define REF_MONITOR 3
#define FIRE_FRAMES 8
#define FIRE_FPS    14.0f
#define LAVA_FPS    10.0f
#define OFF_SIZE    32   /* = the models' refSize: the UVs are baked to it */

static T3DModel *g_firewall, *g_lavapool, *g_monitors;
static sprite_t *g_fire_frames[FIRE_FRAMES];
static surface_t g_lava_ci, g_off, g_off_z;
static uint16_t *g_lava_pals[16];
static T3DViewport g_vp_off;
static T3DMat4FP *g_off_mat;

static KilnTexAnim g_fire = { .mode = KILN_TEXANIM_FLIPBOOK, .ref_id = REF_FIRE };
static KilnTexAnim g_lava = { .mode = KILN_TEXANIM_PALETTE, .ref_id = REF_LAVA };
static KilnTexAnim g_tv = { .mode = KILN_TEXANIM_OFFSCREEN, .ref_id = REF_MONITOR };

/* A CI4 surface of lava cells and sixteen rotations of one 16-colour ramp.
 * Cycling the palette moves the colour through the cells without touching a
 * single index: the whole point of the mode, and 512 bytes of TMEM. The
 * pattern is periodic in 32 texels so the pool's four repeats tile. */
static void lava_setup(void)
{
    g_lava_ci = surface_alloc(FMT_CI4, OFF_SIZE, OFF_SIZE);
    const float k = 6.2832f / 32.0f;
    for (int y = 0; y < OFF_SIZE; y++) {
        uint8_t *row = (uint8_t *)g_lava_ci.buffer + y * g_lava_ci.stride;
        for (int x = 0; x < OFF_SIZE; x++) {
            const float v = fm_sinf(x * k * 2.0f) + fm_sinf(y * k * 2.0f) +
                            fm_sinf((x + y) * k) + fm_cosf((x - y) * k * 3.0f);
            int idx = (int)((v + 4.0f) * 16.0f / 8.0f);
            idx &= 15;
            if (x & 1) row[x / 2] = (row[x / 2] & 0xF0) | idx;
            else       row[x / 2] = (row[x / 2] & 0x0F) | (idx << 4);
        }
    }
    data_cache_hit_writeback(g_lava_ci.buffer, OFF_SIZE * g_lava_ci.stride);

    /* RDP TLUTs are read straight from memory: 8-byte aligned, per rdpq_tex.h. */
    uint16_t *block = malloc_uncached_aligned(8, 16 * 16 * sizeof(uint16_t));
    assertf(block, "texanim-demo: palette alloc");
    uint16_t ramp[16];
    for (int i = 0; i < 16; i++) {
        const float t = 0.5f - 0.5f * fm_cosf(6.2832f * i / 16.0f);   /* 0 .. 1 .. 0 */
        const int r = 70 + (int)(185 * t);
        const int g = (int)(40 + 200 * t * t);
        const int b = (int)(10 + 110 * t * t * t);
        ramp[i] = color_to_packed16(RGBA32(r, g, b, 0xFF));
    }
    for (int p = 0; p < 16; p++) {
        g_lava_pals[p] = block + 16 * p;
        for (int i = 0; i < 16; i++) g_lava_pals[p][i] = ramp[(i + p) % 16];
    }
    g_lava.palette = (typeof(g_lava.palette)){ .indices = &g_lava_ci, .palettes = g_lava_pals,
                                               .pal_count = 16, .colors_per = 16, .fps = LAVA_FPS };
}

/* The monitor wall's picture: the cel torus's model spinning in its own tiny
 * viewport, rendered BEFORE the frame is attached, into a surface the monitors'
 * material then samples. Tiny3D's 06_offscreen does it this way. */
static void offscreen_setup(void)
{
    g_off = surface_alloc(FMT_RGBA16, OFF_SIZE, OFF_SIZE);
    g_off_z = surface_alloc(FMT_RGBA16, OFF_SIZE, OFF_SIZE);
    g_vp_off = t3d_viewport_create();
    t3d_viewport_set_area(&g_vp_off, 0, 0, OFF_SIZE, OFF_SIZE);
    t3d_viewport_set_projection(&g_vp_off, T3D_DEG_TO_RAD(50.0f), 10.0f, 400.0f);
    t3d_viewport_look_at(&g_vp_off, &(T3DVec3){{ 0, 20, -130 }}, &(T3DVec3){{ 0, 0, 0 }},
                         &(T3DVec3){{ 0, 1, 0 }});
    g_off_mat = malloc_uncached(sizeof(T3DMat4FP));
    g_tv.offscreen.surface = &g_off;
}

static void offscreen_render(const T3DModel *model, float radius, float t)
{
    rdpq_attach(&g_off, &g_off_z);
    rdpq_clear(RGBA32(0x10, 0x18, 0x40, 0xFF));
    rdpq_clear_z(ZBUF_MAX);
    t3d_frame_start();
    t3d_viewport_attach(&g_vp_off);
    t3d_light_set_ambient((uint8_t[4]){ 0x40, 0x40, 0x50, 0xFF });
    t3d_light_set_directional(0, (uint8_t[4]){ 0xFF, 0xE8, 0xC0, 0xFF },
                              &(T3DVec3){{ 0.4f, 0.7f, -0.6f }});
    t3d_light_set_count(1);
    const float s = 46.0f / radius;
    t3d_mat4fp_from_srt_euler(g_off_mat, (float[3]){ s, s, s }, (float[3]){ t * 1.3f, t * 0.8f, 0 },
                              (float[3]){ 0, 0, 0 });
    t3d_matrix_push(g_off_mat);
    t3d_model_draw(model);
    t3d_matrix_pop(1);
    rdpq_detach();
}

// ── Cel shade: the ramp the ucode indexes by light intensity ───────────
// CELSHADE_COLOR writes max(r,g,b) of the LIT vertex colour into S, so the
// texel it lands on is a band. Eight texels with S shifted by 7 is the exact
// arrangement Tiny3D's 13_cel_shading ships; 8 bytes of TMEM.
static surface_t g_ramp;
static const uint8_t RAMP[8] = { 0x50, 0x50, 0x50, 0x90, 0x90, 0xC8, 0xC8, 0xFF };

// ── Flag: kiln_deform ───────────────────────────────────────────────────
typedef struct { float amp; float length; } FlagWave;
static FlagWave g_wave;
static KilnDeform g_deform;

/* A travelling wave pinned at the hoist: displacement grows with x, and the
 * normal is the analytic one of z = f(x), so the ripples light correctly
 * instead of shading like a flat sheet. */
static void flag_wave(T3DVertPacked *verts, int count, float time, void *user)
{
    const FlagWave *w = (const FlagWave *)user;
    const float k = 6.2832f * 1.6f / w->length;   /* 1.6 waves along the flag */
    for (int i = 0; i < count; i++) {
        int16_t *pos = t3d_vertbuffer_get_pos(verts, i);
        const float u = (float)pos[0] / w->length;
        const float ph = (float)pos[0] * k - time * 5.0f;
        const float s = fm_sinf(ph), c = fm_cosf(ph);
        pos[2] = (int16_t)(w->amp * u * s);
        const float slope = w->amp * (u * k * c + s / w->length);
        fm_vec3_t n = {{ -slope, 0.0f, 1.0f }};
        fm_vec3_norm(&n, &n);
        *t3d_vertbuffer_get_norm(verts, i) = t3d_vert_pack_normal(&n);
    }
}

// ── Blob: kiln_morph ────────────────────────────────────────────────────
#define MORPH_TARGETS 4
static KilnMorph g_morph;
static T3DVertPacked *g_targets[MORPH_TARGETS];

static uint32_t pack_rgb(float r, float g, float b)
{
    const int R = (int)(r > 255 ? 255 : r), G = (int)(g > 255 ? 255 : g), B = (int)(b > 255 ? 255 : b);
    return ((uint32_t)R << 24) | ((uint32_t)G << 16) | ((uint32_t)B << 8) | 0xFF;
}

/* Four shapes with the sphere's topology: the sphere itself, a tall teal
 * capsule, a flattened magenta disc and a lumpy orange gourd. Colours are
 * shaded by height so a blend between two of them reads as a gradient moving,
 * which is exactly where packed-word colour blending used to garble. */
static void morph_setup(void)
{
    const int vc = g_blob->totalVertCount;
    const size_t sz = sizeof(T3DVertPacked) * ((vc + 1) / 2);
    const T3DVertPacked *base = t3d_model_get_vertices(g_blob);
    const float r = (float)(g_blob->aabbMax[1] > 1 ? g_blob->aabbMax[1] : 1);

    for (int t = 0; t < MORPH_TARGETS; t++) {
        g_targets[t] = malloc(sz);
        assertf(g_targets[t], "texanim-demo: morph target alloc");
        memcpy(g_targets[t], base, sz);
        if (t == 0) continue;
        for (int i = 0; i < vc; i++) {
            int16_t *p = t3d_vertbuffer_get_pos(g_targets[t], i);
            const float x = p[0], y = p[1], z = p[2];
            const float h = 0.5f + 0.5f * y / r;                /* 0 bottom .. 1 top */
            float sx = 1, sy = 1, lr, lg, lb;
            if (t == 1) {                 /* capsule */
                sx = 0.72f; sy = 1.45f;
                lr = 0;   lg = 150 + 95 * h; lb = 130 + 82 * h;
            } else if (t == 2) {          /* disc */
                sx = 1.35f; sy = 0.5f;
                lr = 150 + 105 * h; lg = 40 + 40 * h; lb = 90 + 40 * h;
            } else {                      /* gourd */
                const float lump = 1.0f + 0.22f * fm_sinf(3.0f * x / r) * fm_cosf(3.0f * z / r);
                sx = lump; sy = 1.0f + 0.25f * fm_sinf(2.0f * y / r);
                lr = 200 + 55 * h; lg = 110 + 70 * h; lb = 30 + 20 * h;
            }
            p[0] = (int16_t)(x * sx);
            p[1] = (int16_t)(y * sy);
            p[2] = (int16_t)(z * sx);
            *t3d_vertbuffer_get_color(g_targets[t], i) = pack_rgb(lr, lg, lb);
        }
    }
    kiln_morph_init(&g_morph, g_blob, g_targets, MORPH_TARGETS, 2, 1);
}

// ── Helpers ─────────────────────────────────────────────────────────────
static T3DModel *load_model(const char *path)
{
    T3DModel *m = t3d_model_load(path);
    assertf(m, "texanim-demo: %s did not load", path);
    return m;
}

static float approach(float v, float to, float rate)
{
    return v + (to - v) * (rate > 1.0f ? 1.0f : rate);
}

static fm_vec3_t vlerp(fm_vec3_t a, fm_vec3_t b, float t)
{
    return (fm_vec3_t){{ a.v[0] + (b.v[0] - a.v[0]) * t,
                         a.v[1] + (b.v[1] - a.v[1]) * t,
                         a.v[2] + (b.v[2] - a.v[2]) * t }};
}

/* Model radius from its own bounding box, so no exhibit size is typed here. */
static float model_radius(const T3DModel *m)
{
    float r = 1.0f;
    for (int a = 0; a < 3; a++) {
        const float lo = -(float)m->aabbMin[a], hi = (float)m->aabbMax[a];
        if (lo > r) r = lo;
        if (hi > r) r = hi;
    }
    return r;
}

static void draw_cel_torus(bool on)
{
    if (!on) {
        t3d_model_draw(g_torus);
        return;
    }
    /* The model's own material would put SHADE back, so its objects are drawn
     * directly under state set here — Tiny3D's own cel example does the same. */
    rdpq_mode_begin();
    rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, PRIM, 0), (0, 0, 0, 1)));
    rdpq_mode_filter(FILTER_POINT);
    rdpq_mode_end();
    rdpq_set_prim_color(RGBA32(0xB0, 0x78, 0xFF, 0xFF));
    rdpq_tex_upload(TILE0, &g_ramp, &(rdpq_texparms_t){
        .s = { .scale_log = 7, .repeats = 1 }, .t = { .repeats = 1 } });
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_TEXTURED | T3D_FLAG_DEPTH | T3D_FLAG_CULL_BACK);
    kiln_vfx_set(KILN_VFX_CELSHADE_COLOR, 0, 0);
    T3DModelIter it = t3d_model_iter_create(g_torus, T3D_CHUNK_TYPE_OBJECT);
    while (t3d_model_iter_next(&it)) t3d_model_draw_object(it.object, NULL);

    /* The ink: the same mesh again, front faces culled, pushed out a few
     * pixels in screen space by the ucode and drawn in a flat colour.
     * SHADED stays on although the combiner ignores shade: the RSP writes
     * the fog factor into shade alpha, and without it every ink pixel reads
     * as fully fogged — the outline came out sky-coloured in Ares. */
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_set_prim_color(RGBA32(0x10, 0x08, 0x20, 0xFF));
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_CULL_FRONT);
    kiln_vfx_set(KILN_VFX_OUTLINE, 16, 16);
    it = t3d_model_iter_create(g_torus, T3D_CHUNK_TYPE_OBJECT);
    while (t3d_model_iter_next(&it)) t3d_model_draw_object(it.object, NULL);
    kiln_vfx_clear();
}

/* Back to kiln_scene_begin's untextured state. */
static void plain_state(void)
{
    rdpq_mode_begin();
    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    rdpq_mode_filter(FILTER_BILINEAR);
    rdpq_mode_end();
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH);
}

int main(void)
{
    debug_init_isviewer();
    kiln_engine_init(RESOLUTION_320x240);
    dfs_init(DFS_DEFAULT_LOCATION);
    asset_init_compression(2);   /* mkBlenderModel's mkasset -c 2 */
    joypad_init();
    kiln_input_init();

    g_floor = load_model("rom:/models/tilefloor.t3dm");
    g_env   = load_model("rom:/models/envsphere.t3dm");
    g_torus = load_model("rom:/models/celtorus.t3dm");
    g_flag  = load_model("rom:/models/flag.t3dm");
    g_blob  = load_model("rom:/models/blob.t3dm");
    g_firewall = load_model("rom:/models/firewall.t3dm");
    g_lavapool = load_model("rom:/models/lavapool.t3dm");
    g_monitors = load_model("rom:/models/monitors.t3dm");
    for (int i = 0; i < FIRE_FRAMES; i++) {
        char path[48];
        snprintf(path, sizeof(path), "rom:/textures/fire%d.rgba16.sprite", i);
        g_fire_frames[i] = sprite_load(path);
        assertf(g_fire_frames[i], "texanim-demo: %s did not load", path);
    }
    g_fire.flipbook = (typeof(g_fire.flipbook)){ .frames = g_fire_frames, .frame_count = FIRE_FRAMES,
                                                 .fps = FIRE_FPS };
    lava_setup();
    offscreen_setup();

    g_ramp = surface_alloc(FMT_I8, 8, 1);
    memcpy(g_ramp.buffer, RAMP, sizeof(RAMP));
    data_cache_hit_writeback(g_ramp.buffer, sizeof(RAMP));

    g_wave.length = (float)(g_flag->aabbMax[0] > 1 ? g_flag->aabbMax[0] : 1);
    g_wave.amp = 0.0f;
    kiln_deform_init(&g_deform, g_flag, flag_wave, &g_wave, 2, 2);
    morph_setup();

    // ── stage ───────────────────────────────────────────────────────────
    KilnScene scene;
    kiln_scene_init(&scene);
    kiln_prim_stage(&scene, RGBA32(0x60, 0x78, 0xA0, 0xFF), 280.0f, 600.0f);
    scene.fov_deg = 62.0f;
    scene.near_z = 8.0f;
    scene.far_z = 700.0f;

    KilnPrim plinth, pole, cap;
    kiln_prim_box(&plinth, (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 20, PLINTH_TOP / 2, 20 }},
                  kiln_prim_rgba(0xC8, 0xB8, 0xA0), kiln_prim_rgba(0x8C, 0x7C, 0x6C),
                  kiln_prim_rgba(0x40, 0x38, 0x30));
    kiln_prim_box(&pole, (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 2, POLE_H / 2, 2 }},
                  kiln_prim_rgba(0xE0, 0xE0, 0xE8), kiln_prim_rgba(0xA0, 0xA4, 0xB0),
                  kiln_prim_rgba(0x60, 0x60, 0x68));
    kiln_prim_box(&cap, (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 4, 3, 4 }},
                  kiln_prim_rgba(0xFF, 0xC8, 0x50), kiln_prim_rgba(0xD0, 0x98, 0x30),
                  kiln_prim_rgba(0x80, 0x60, 0x20));

    /* The new exhibits' furniture, built where they stand. A reference panel
     * is the checker model's front quad, 22 units a side at QUAD_S and 24
     * in front of its origin; the rear quad sits inside the box behind it. */
    const float QUAD_S = 22.0f / 76.8f;
    const fm_vec3_t fp = SPOT[MODE_FLIP], lp = SPOT[MODE_PAL], mp = SPOT[MODE_OFFSCR];
    KilnPrim hearth, mantel, bezel, stand, basin, rims[4];
    kiln_prim_box(&hearth, (fm_vec3_t){{ fp.v[0], 27, fp.v[2] }}, (fm_vec3_t){{ 30, 27, 21 }},
                  kiln_prim_rgba(0x8C, 0x64, 0x50), kiln_prim_rgba(0x6C, 0x48, 0x3A),
                  kiln_prim_rgba(0x30, 0x20, 0x18));
    kiln_prim_box(&mantel, (fm_vec3_t){{ fp.v[0], 57, fp.v[2] + 4 }}, (fm_vec3_t){{ 36, 3, 26 }},
                  kiln_prim_rgba(0xD8, 0xC8, 0xB0), kiln_prim_rgba(0xA0, 0x90, 0x78),
                  kiln_prim_rgba(0x50, 0x44, 0x38));
    kiln_prim_box(&bezel, (fm_vec3_t){{ mp.v[0], 34, mp.v[2] }}, (fm_vec3_t){{ 21, 27, 27 }},
                  kiln_prim_rgba(0x38, 0x3C, 0x48), kiln_prim_rgba(0x24, 0x28, 0x30),
                  kiln_prim_rgba(0x14, 0x14, 0x18));
    kiln_prim_box(&stand, (fm_vec3_t){{ mp.v[0], 3.5f, mp.v[2] }}, (fm_vec3_t){{ 14, 3.5f, 18 }},
                  kiln_prim_rgba(0x60, 0x64, 0x70), kiln_prim_rgba(0x40, 0x44, 0x50),
                  kiln_prim_rgba(0x20, 0x20, 0x28));
    kiln_prim_box(&basin, (fm_vec3_t){{ lp.v[0], 0.6f, lp.v[2] }}, (fm_vec3_t){{ 34, 0.6f, 34 }},
                  kiln_prim_rgba(0x30, 0x14, 0x10), kiln_prim_rgba(0x30, 0x14, 0x10),
                  kiln_prim_rgba(0x10, 0x08, 0x08));
    for (int i = 0; i < 4; i++) {
        const int along_x = i < 2;
        const float o = (i % 2 ? 1.0f : -1.0f) * 34.0f;
        kiln_prim_box(&rims[i],
                      (fm_vec3_t){{ lp.v[0] + (along_x ? 0 : o), 3, lp.v[2] + (along_x ? o : 0) }},
                      (fm_vec3_t){{ along_x ? 37.0f : 3.0f, 3, along_x ? 3.0f : 37.0f }},
                      kiln_prim_rgba(0x7C, 0x74, 0x6C), kiln_prim_rgba(0x5C, 0x54, 0x4C),
                      kiln_prim_rgba(0x30, 0x2C, 0x28));
    }

    const float env_s = 22.0f / model_radius(g_env);
    const float tor_s = 24.0f / model_radius(g_torus);
    const float blob_s = 20.0f / model_radius(g_blob);
    const float flag_s = FLAG_LEN / g_wave.length;

    KilnTransform xf_floor, xf_plinth[4], xf_pole, xf_cap, xf_env, xf_torus, xf_flag, xf_blob;
    kiln_transform_init(&xf_floor);
    for (int i = 0; i < 4; i++) {
        kiln_transform_init(&xf_plinth[i]);
        xf_plinth[i].pos = (fm_vec3_t){{ SPOT[MODE_ENVMAP + i].v[0], PLINTH_TOP / 2 - 1,
                                         SPOT[MODE_ENVMAP + i].v[2] }};
    }
    kiln_transform_init(&xf_pole);
    kiln_transform_init(&xf_cap);
    kiln_transform_init(&xf_env);
    kiln_transform_init(&xf_torus);
    kiln_transform_init(&xf_flag);
    kiln_transform_init(&xf_blob);
    KilnTransform xf_fire, xf_tv, xf_lava;
    kiln_transform_init(&xf_fire);
    kiln_transform_init(&xf_tv);
    kiln_transform_init(&xf_lava);
    xf_fire.pos = (fm_vec3_t){{ fp.v[0], 24, fp.v[2] }};
    xf_fire.scale = (fm_vec3_t){{ QUAD_S, QUAD_S, QUAD_S }};
    xf_tv.pos = (fm_vec3_t){{ mp.v[0], 34, mp.v[2] }};
    xf_tv.scale = (fm_vec3_t){{ QUAD_S, QUAD_S, QUAD_S }};
    /* The panel faces +Z; libdragon's axis-angle about +Y turns +Z to
     * (-sin a, 0, cos a), so -PI/2 faces it out along +X. */
    xf_tv.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
    xf_tv.rot_angle = -1.5708f;
    const float LAVA_S = 31.0f / 153.6f;
    xf_lava.pos = (fm_vec3_t){{ lp.v[0], 1.4f, lp.v[2] }};
    xf_lava.scale = (fm_vec3_t){{ LAVA_S, LAVA_S, LAVA_S }};
    /* The flag plinth holds a pole; the banner hangs from just under its top. */
    const fm_vec3_t fs = SPOT[MODE_FLAG];
    xf_pole.pos = (fm_vec3_t){{ fs.v[0], PLINTH_TOP + POLE_H / 2, fs.v[2] }};
    xf_cap.pos = (fm_vec3_t){{ fs.v[0], PLINTH_TOP + POLE_H + 3, fs.v[2] }};
    xf_flag.pos = (fm_vec3_t){{ fs.v[0] - 2, PLINTH_TOP + POLE_H - 3 - g_flag->aabbMax[1] * flag_s,
                                fs.v[2] }};
    /* Turned half round: the banner flies toward -X (screen right from the
     * starting view) with its front, engine +Z in the model, facing -Z. */
    xf_flag.scale = (fm_vec3_t){{ flag_s, flag_s, flag_s }};
    xf_flag.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
    xf_flag.rot_angle = 3.1416f;
    xf_env.scale = (fm_vec3_t){{ env_s, env_s, env_s }};
    xf_torus.scale = (fm_vec3_t){{ tor_s, tor_s, tor_s }};
    xf_torus.rot_axis = (fm_vec3_t){{ 1.0f, 0.35f, 0.0f }};
    fm_vec3_norm(&xf_torus.rot_axis, &xf_torus.rot_axis);
    xf_blob.scale = (fm_vec3_t){{ blob_s, blob_s, blob_s }};
    xf_blob.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};

    /* What each exhibit's camera looks at: the subject's middle. */
    fm_vec3_t focus[MODE_COUNT];
    focus[MODE_TOUR] = (fm_vec3_t){{ 0, 24, 0 }};
    focus[MODE_SCROLL] = (fm_vec3_t){{ 0, 0, 20 }};
    for (int i = MODE_ENVMAP; i < MODE_COUNT; i++)
        focus[i] = (fm_vec3_t){{ SPOT[i].v[0], SUBJECT_Y, SPOT[i].v[2] }};
    focus[MODE_FLAG] = (fm_vec3_t){{ fs.v[0] - FLAG_LEN / 2, PLINTH_TOP + POLE_H * 0.62f, fs.v[2] }};
    focus[MODE_FLIP] = (fm_vec3_t){{ fp.v[0], 26, fp.v[2] + 20 }};
    focus[MODE_PAL] = (fm_vec3_t){{ lp.v[0], 2, lp.v[2] }};
    focus[MODE_OFFSCR] = (fm_vec3_t){{ mp.v[0] + 20, 34, mp.v[2] }};

    // ── state ───────────────────────────────────────────────────────────
    Mode mode = MODE_TOUR;
    switch (KILN_JUMP) {
    case JUMP_SCROLL: mode = MODE_SCROLL; break;
    case JUMP_ENVMAP: mode = MODE_ENVMAP; break;
    case JUMP_CEL:    mode = MODE_CEL;    break;
    case JUMP_FLAG:   mode = MODE_FLAG;   break;
    case JUMP_MORPH:  mode = MODE_MORPH;  break;
    case JUMP_FLIP:   mode = MODE_FLIP;   break;
    case JUMP_PAL:    mode = MODE_PAL;    break;
    case JUMP_OFFSCR: mode = MODE_OFFSCR; break;
    default: kiln_input_set_attract(1, &ATTRACT, 180); break;
    }
    bool on[MODE_COUNT];
    for (int i = 0; i < MODE_COUNT; i++) on[i] = true;
    float t = 0.0f;
    float morph_phase = 0.0f;
    float blend_amt = 1.0f;          /* morph: 1 = cycling targets, 0 = sphere */
    fm_vec3_t cam_pos = {{ 0, 175, -270 }}, cam_look = {{ 0, 24, 0 }};

    for (;;) {
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);
        if (in->edges & KILN_BTN_DR) mode = (Mode)((mode + 1) % MODE_COUNT);
        if (in->edges & KILN_BTN_DL) mode = (Mode)((mode + MODE_COUNT - 1) % MODE_COUNT);
        if (in->edges & KILN_BTN_A) {
            if (mode == MODE_TOUR) {
                const bool v = !on[MODE_TOUR];
                for (int i = 0; i < MODE_COUNT; i++) on[i] = v;
            } else {
                on[mode] = !on[mode];
            }
        }
        t += DT;

        // ── effects ─────────────────────────────────────────────────────
        g_scroll.scroll.s_speed = on[MODE_SCROLL] ? SCROLL_S : 0.0f;
        g_scroll.scroll.t_speed = on[MODE_SCROLL] ? SCROLL_T : 0.0f;
        /* Off freezes the flipbook and the palette on their current frame, and
         * the monitors on their last picture. */
        g_fire.flipbook.fps = on[MODE_FLIP] ? FIRE_FPS : 0.0f;
        if (on[MODE_FLIP]) kiln_texanim_update(&g_fire, 1, DT);
        if (on[MODE_PAL]) kiln_texanim_update(&g_lava, 1, DT);
        kiln_texanim_update(&g_scroll, 1, DT);
        if (g_scroll.scroll.s_offset >= CHECKER_PERIOD) g_scroll.scroll.s_offset -= CHECKER_PERIOD;
        if (g_scroll.scroll.t_offset >= CHECKER_PERIOD) g_scroll.scroll.t_offset -= CHECKER_PERIOD;

        g_wave.amp = approach(g_wave.amp, on[MODE_FLAG] ? g_wave.length * 0.09f : 0.0f, 3.0f * DT);
        kiln_deform_update(&g_deform, DT);

        blend_amt = approach(blend_amt, on[MODE_MORPH] ? 1.0f : 0.0f, 3.0f * DT);
        if (on[MODE_MORPH]) morph_phase += DT * 0.45f;
        if (morph_phase >= (float)MORPH_TARGETS) morph_phase -= (float)MORPH_TARGETS;
        {
            int k = (int)morph_phase;
            float s = morph_phase - (float)k;
            /* hold each shape, then ease into the next */
            s = s < 0.4f ? 0.0f : (s - 0.4f) / 0.6f;
            s = s * s * (3.0f - 2.0f * s);
            for (int i = 0; i < MORPH_TARGETS; i++) g_morph.weights[i] = 0.0f;
            g_morph.weights[k] += (1.0f - s) * blend_amt;
            g_morph.weights[(k + 1) % MORPH_TARGETS] += s * blend_amt;
            g_morph.weights[0] += 1.0f - blend_amt;
        }
        kiln_morph_update(&g_morph, DT);

        xf_env.pos = (fm_vec3_t){{ SPOT[MODE_ENVMAP].v[0], SUBJECT_Y + 4 * fm_sinf(t * 1.7f),
                                   SPOT[MODE_ENVMAP].v[2] }};
        xf_torus.pos = focus[MODE_CEL];
        xf_torus.rot_angle = t * 0.9f;
        xf_blob.pos = focus[MODE_MORPH];
        xf_blob.rot_angle = t * 0.5f;

        // ── camera ──────────────────────────────────────────────────────
        const fm_vec3_t want_look = focus[mode];
        fm_vec3_t want_pos;
        if (mode == MODE_TOUR) {
            const float a = 3.1416f + t * 0.18f;
            want_pos = (fm_vec3_t){{ fm_sinf(a) * 270, 175, fm_cosf(a) * 270 }};
        } else if (mode == MODE_SCROLL) {
            const float a = 3.1416f + 0.35f * fm_sinf(t * 0.3f);
            want_pos = (fm_vec3_t){{ fm_sinf(a) * 100, 115, fm_cosf(a) * 100 }};
        } else if (mode == MODE_PAL) {
            /* The pool is flat: look down into it from above its near rim. */
            const float sw = 0.35f * fm_sinf(t * 0.4f);
            want_pos = (fm_vec3_t){{ want_look.v[0] + fm_sinf(sw) * 60, 78,
                                     want_look.v[2] - fm_cosf(sw) * 60 }};
        } else {
            /* Stand outside the ring, looking in, swinging +-30 degrees. */
            fm_vec3_t out = {{ SPOT[mode].v[0], 0, SPOT[mode].v[2] }};
            fm_vec3_norm(&out, &out);
            const float sw = 0.5f * fm_sinf(t * 0.4f);
            const float cs = fm_cosf(sw), sn = fm_sinf(sw);
            const float ox = out.v[0] * cs - out.v[2] * sn, oz = out.v[0] * sn + out.v[2] * cs;
            const float dist = mode == MODE_FLAG ? 135.0f : 110.0f;
            want_pos = (fm_vec3_t){{ want_look.v[0] + ox * dist, want_look.v[1] + 34,
                                     want_look.v[2] + oz * dist }};
        }
        const float follow = KILN_JUMP != JUMP_NONE ? 1.0f : 2.2f * DT;
        cam_look = vlerp(cam_look, want_look, follow);
        cam_pos = vlerp(cam_pos, want_pos, follow);
        scene.cam_pos = cam_pos;
        scene.cam_target = cam_look;
        kiln_scene_update(&scene);

        // ── offscreen, before the frame is attached ─────────────────────
        if (on[MODE_OFFSCR]) offscreen_render(g_torus, model_radius(g_torus), t);

        // ── 3D ──────────────────────────────────────────────────────────
        kiln_frame_begin();
        kiln_scene_begin(&scene);

        for (int i = 0; i < 4; i++) {
            kiln_transform_push(&xf_plinth[i]);
            kiln_prim_draw(&plinth);
            kiln_transform_pop();
        }
        kiln_transform_push(&xf_pole); kiln_prim_draw(&pole); kiln_transform_pop();
        kiln_transform_push(&xf_cap);  kiln_prim_draw(&cap);  kiln_transform_pop();
        kiln_prim_draw(&hearth);
        kiln_prim_draw(&mantel);
        kiln_prim_draw(&bezel);
        kiln_prim_draw(&stand);
        kiln_prim_draw(&basin);
        for (int i = 0; i < 4; i++) kiln_prim_draw(&rims[i]);

        /* Models last: each one's material leaves its combiner and draw flags
         * behind, which is wrong for the primitives above. */
        kiln_transform_push(&xf_floor);
        kiln_texanim_draw(g_floor, &g_scroll, 1);
        kiln_transform_pop();

        kiln_transform_push(&xf_env);
        if (on[MODE_ENVMAP]) kiln_vfx_set(KILN_VFX_SPHERICAL_UV, 32, 32);
        t3d_model_draw(g_env);
        kiln_vfx_clear();
        kiln_transform_pop();

        kiln_transform_push(&xf_torus);
        draw_cel_torus(on[MODE_CEL]);
        kiln_transform_pop();

        kiln_transform_push(&xf_flag);
        kiln_deform_draw(&g_deform);
        kiln_transform_pop();

        kiln_transform_push(&xf_blob);
        kiln_morph_draw(&g_morph);
        kiln_transform_pop();

        kiln_transform_push(&xf_fire);
        kiln_texanim_draw(g_firewall, &g_fire, 1);
        kiln_transform_pop();

        kiln_transform_push(&xf_lava);
        kiln_texanim_draw(g_lavapool, &g_lava, 1);
        kiln_transform_pop();

        kiln_transform_push(&xf_tv);
        kiln_texanim_draw(g_monitors, &g_tv, 1);
        kiln_transform_pop();

        plain_state();

        // ── 2D ──────────────────────────────────────────────────────────
        kiln_gui_begin();
        const color_t ink = RGBA32(0xE8, 0xE8, 0xF0, 0xFF);
        const color_t teal = RGBA32(0x00, 0xF5, 0xD4, 0xFF);
        const color_t dim = RGBA32(0x98, 0xA0, 0xB8, 0xFF);
        const color_t navy = RGBA32(0x0C, 0x10, 0x1C, 0xFF);

        if (mode == MODE_TOUR) {
            for (int i = MODE_ENVMAP; i < MODE_COUNT; i++) {
                const float lift = i == MODE_FLAG ? PLINTH_TOP + POLE_H + 16
                                 : i == MODE_FLIP ? 72.0f : i == MODE_PAL ? 16.0f
                                 : i == MODE_OFFSCR ? 70.0f : SUBJECT_Y + 34;
                const fm_vec3_t p = {{ SPOT[i].v[0], lift, SPOT[i].v[2] }};
                int sx, sy;
                if (kiln_scene_project(&scene, p, SCREEN_W, SCREEN_H, &sx, &sy) &&
                    sx > 30 && sx < SCREEN_W - 30 && sy > 90 && sy < SCREEN_H - 34) {
                    const int w = (int)strlen(MODE_NAME[i]) * 6;
                    kiln_gui_panel(sx - w / 2 - 3, sy - 10, w + 6, 13, navy, teal);
                    kiln_gui_text(sx - w / 2, sy, ink, "%s", MODE_NAME[i]);
                }
            }
        }

        kiln_gui_panel(8, 8, 196, 58, navy, teal);
        kiln_gui_text(14, 21, teal, "KILN TEXANIM + VANIM");
        kiln_gui_text(14, 33, ink, "%-10s effect %s", MODE_NAME[mode], on[mode] ? "ON" : "OFF");
        kiln_gui_text(14, 45, dim, "%s", MODE_API[mode]);
        switch (mode) {
        case MODE_SCROLL:
            kiln_gui_text(14, 57, dim, "tile translate %4.1f %4.1f",
                          g_scroll.scroll.s_offset, g_scroll.scroll.t_offset);
            break;
        case MODE_ENVMAP: kiln_gui_text(14, 57, dim, "UVs from view-space normals"); break;
        case MODE_CEL:    kiln_gui_text(14, 57, dim, "8x1 ramp, then OUTLINE pass"); break;
        case MODE_FLAG:
            kiln_gui_text(14, 57, dim, "wave %4.1f  buffer %d", g_wave.amp, g_deform.current_buffer);
            break;
        case MODE_MORPH:
            kiln_gui_text(14, 57, dim, "w %.2f %.2f %.2f %.2f", g_morph.weights[0],
                          g_morph.weights[1], g_morph.weights[2], g_morph.weights[3]);
            break;
        case MODE_FLIP:
            kiln_gui_text(14, 57, dim, "frame %d of %d, ref %d",
                          (int)(g_fire.flipbook.time * FIRE_FPS) % FIRE_FRAMES + 1, FIRE_FRAMES, REF_FIRE);
            break;
        case MODE_PAL:
            kiln_gui_text(14, 57, dim, "CI4, palette %2d/16, ref %d",
                          (int)(g_lava.palette.time * LAVA_FPS) % 16 + 1, REF_LAVA);
            break;
        case MODE_OFFSCR: kiln_gui_text(14, 57, dim, "32x32 RGBA16 render, ref %d", REF_MONITOR); break;
        default: kiln_gui_text(14, 57, dim, "D-pad picks an exhibit"); break;
        }
        for (int i = 0; i < MODE_COUNT; i++) {
            kiln_gui_rect(14 + i * 14, 70, 10, 4,
                          i == (int)mode ? teal : RGBA32(0x30, 0x34, 0x44, 0xFF));
        }

        if (kiln_input_scripted(1)) {
            kiln_gui_panel(SCREEN_W - 58, SCREEN_H - 44, 50, 16, RGBA32(0xC0, 0x30, 0x60, 0xFF),
                           RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
            kiln_gui_text(SCREEN_W - 49, SCREEN_H - 32, RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "DEMO");
        }
        kiln_gui_panel(8, SCREEN_H - 24, SCREEN_W - 16, 16, navy, RGBA32(0x8B, 0x5C, 0xF6, 0xFF));
        kiln_gui_text(14, SCREEN_H - 12, ink, "D< D> exhibit    A effect on/off");

        kiln_gui_end();
        kiln_frame_end();
    }
}
