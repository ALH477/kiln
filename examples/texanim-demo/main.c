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
//   MORPH      blob              kiln_morph — four CPU targets, colour per channel
//
// The camera flies to the exhibit in focus and the HUD names it and the call
// behind it. D-pad left/right picks an exhibit; A turns its effect off and on,
// so the difference is visible in place. Idle 3 s: an attract tape tours all
// of them, switching each effect off and back on as it passes.
//
// The host cannot draw any of this (tile callbacks, vertex FX and vertex
// placeholders are console-only), so this ROM is verified in Ares.
//
// Jumps: .#texanim-demo-{scroll,envmap,cel,flag,morph} boot focused on one
// exhibit with its effect on, and stay there.

#include <libdragon.h>
#include <t3d/t3dmodel.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_prim.h>
#include <kiln/kiln_texanim.h>
#include <kiln/kiln_vanim.h>

#include <string.h>

enum { JUMP_NONE, JUMP_SCROLL, JUMP_ENVMAP, JUMP_CEL, JUMP_FLAG, JUMP_MORPH };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W 320
#define SCREEN_H 240
#define DT (1.0f / 60.0f)

/* The checker texture is 32 texels a side; wrapping the scroll offset at its
 * period keeps the tile translate small forever with no visible jump. */
#define CHECKER_PERIOD 32.0f

typedef enum { MODE_TOUR, MODE_SCROLL, MODE_ENVMAP, MODE_CEL, MODE_FLAG, MODE_MORPH, MODE_COUNT } Mode;

static const char *const MODE_NAME[MODE_COUNT] = {
    "TOUR", "UV SCROLL", "ENV MAP", "CEL SHADE", "FLAG", "MORPH",
};
static const char *const MODE_API[MODE_COUNT] = {
    "every effect at once",
    "kiln_texanim SCROLL",
    "kiln_vfx SPHERICAL_UV",
    "kiln_vfx CELSHADE_COLOR",
    "kiln_deform, 2 buffers",
    "kiln_morph, 4 targets",
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
static KilnTexAnim g_scroll = {
    .mode = KILN_TEXANIM_SCROLL,
    .scroll = { .s_speed = 10.0f, .t_speed = 5.0f },
};

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

    // ── state ───────────────────────────────────────────────────────────
    Mode mode = MODE_TOUR;
    switch (KILN_JUMP) {
    case JUMP_SCROLL: mode = MODE_SCROLL; break;
    case JUMP_ENVMAP: mode = MODE_ENVMAP; break;
    case JUMP_CEL:    mode = MODE_CEL;    break;
    case JUMP_FLAG:   mode = MODE_FLAG;   break;
    case JUMP_MORPH:  mode = MODE_MORPH;  break;
    default: kiln_input_set_attract(1, &ATTRACT, 180); break;
    }
    bool on[MODE_COUNT] = { true, true, true, true, true, true };
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
        g_scroll.scroll.s_speed = on[MODE_SCROLL] ? 10.0f : 0.0f;
        g_scroll.scroll.t_speed = on[MODE_SCROLL] ? 5.0f : 0.0f;
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

        plain_state();

        // ── 2D ──────────────────────────────────────────────────────────
        kiln_gui_begin();
        const color_t ink = RGBA32(0xE8, 0xE8, 0xF0, 0xFF);
        const color_t teal = RGBA32(0x00, 0xF5, 0xD4, 0xFF);
        const color_t dim = RGBA32(0x98, 0xA0, 0xB8, 0xFF);
        const color_t navy = RGBA32(0x0C, 0x10, 0x1C, 0xFF);

        if (mode == MODE_TOUR) {
            for (int i = MODE_ENVMAP; i < MODE_COUNT; i++) {
                const float lift = i == MODE_FLAG ? PLINTH_TOP + POLE_H + 16 : SUBJECT_Y + 34;
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
