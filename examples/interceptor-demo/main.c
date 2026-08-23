// SPDX-License-Identifier: MIT
//
// Interceptor demo ROM — one screen that tries to do everything the engine
// can do in one go, so the .t3dm hero prop has somewhere to live that
// exercises every layer.
//
//   models/interceptor.t3dm   -> the hero prop (tools/blender/interceptor.py)
//   sfx/blip.wav64            -> SFX (re-used as engine whoosh)
//   music/test.xm64           -> music
//
// ── The story the demo tells ────────────────────────────────────────────
// Title screen (~3 s) carries the project's credit text: developer ALH477,
// MIT license, "not sponsored or endorsed by ModRetro", and the lineage
// tagline — a mix of the Ocarina of Time and IdTech4 engine for the M64,
// processing your assets and code like IdTech does. After the title, a
// 3-mode flight loop runs forever:
//
//   MODE_PARKED      ship yaws on the spot, camera does a slow 360° orbit,
//                    engine glow visible from all angles. Music plays here.
//   MODE_BANKED      ship follows a figure-8, banks into the turn, engine
//                    streaks trail from both nacelles.
//   MODE_CINEMATIC   ship pulls away along a curved arc, streaks at full
//                    brightness, SFX blips punctuate the dolly-out.
//
// Throughout all three modes: 30 static star cubes scatter for parallax, a
// HUD with the lineage block stays in the top-left, the mode label in the
// top-right, and a credit strip across the bottom. The engine glow pulse
// is a slow sine + a sharp on-transition pulse, premultiplied into the
// streak vertex colour so it reads additive against the dark starfield
// without touching the RDP blender.

#include <libdragon.h>
#include <t3d/t3dmodel.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_audio.h>

#include <malloc.h>

#define SCREEN_W 320
#define SCREEN_H 240
#define SAMPLE_RATE 32000
#define DT (1.0f / 60.0f)

#define STAR_COUNT 30
#define STREAK_BASE_LEN 1.5f    /* model units (engine mouth -> trail rear) */
#define STREAK_BASE_A   200     /* peak alpha before breathing modulation */

/* ── Mode + timing state ──────────────────────────────────────────────── */

enum {
    MODE_PARKED = 0,
    MODE_BANKED = 1,
    MODE_CINEMATIC = 2,
    MODE_COUNT
};

static const float MODE_PERIOD[MODE_COUNT] = { 8.0f, 8.0f, 8.0f };
static const char  MODE_NAME[MODE_COUNT][12] = { "PARKED", "BANKED", "CINEMATIC" };

static int   mode         = MODE_PARKED;
static float time_in_mode = 0.0f;
static float total_t      = 0.0f;
/* pulse_t starts high so the first frame after the title has no sharp
 * pulse; it resets to 0 on every mode transition. */
static float pulse_t      = 99.0f;
static float blip_beat_t  = 0.0f;
static bool  in_title     = true;
static float title_t      = 0.0f;
static bool  blip_armed   = false;   /* one blip per mode entry */
static bool  blip_cine_armed = false; /* one blip at time_in_mode == 1s in CINEMATIC */

/* ── Engine mouth local position (model space, see interceptor.py) ─────
 *
 * The model is exported nose-at -Z, up at +Y, wings at ±X. The two
 * nacelle centres are at x = ±0.80, y = -2.55 (the rearmost station).
 * The streak ribbon's front verts anchor at the throat, rear verts
 * trail along -Z. Half-width is the ribbon's spread perpendicular to
 * the trail.
 */
#define ENGINE_OFFSET_Y (-2.55f)
#define ENGINE_OFFSET_Z 0.0f

/* ── Asset / engine globals ──────────────────────────────────────────── */

static T3DModel    *g_ship;
static int          g_blip   = -1;
static int          g_music  = -1;
static KilnScene     g_scene;
static KilnTransform g_ship_xform;

/* ── Streak ribbon (2 triangles, 4 verts = 2 packed structs) ─────────── */

/* Two ribbons, one per engine mouth (left nacelle at x = -0.80, right
 * at x = +0.80, both at y = -2.55 in model space). The front verts stay
 * anchored at the throat; the rear verts' z is rewritten every frame
 * to set the trail length. The half-width is the ribbon's spread
 * perpendicular to the trail direction. */
static T3DVertPacked g_streak_ribbon[2][2];

static const float NACELLE_X[2] = { -0.80f, 0.80f };
static const float STREAK_HALF_WIDTH = 0.06f;

static void make_streak_ribbon(void)
{
    fm_vec3_t n = {{ 0.0f, 0.0f, 1.0f }};
    uint16_t np = t3d_vert_pack_normal(&n);
    for (int e = 0; e < 2; e++) {
        float cx = NACELLE_X[e];
        /* Front verts: at the throat, slightly spread in X. */
        g_streak_ribbon[e][0].posA[0] = (int16_t)((cx - STREAK_HALF_WIDTH) * 64.0f);
        g_streak_ribbon[e][0].posA[1] = (int16_t)(ENGINE_OFFSET_Y * 64.0f);
        g_streak_ribbon[e][0].posA[2] = (int16_t)(ENGINE_OFFSET_Z * 64.0f);
        g_streak_ribbon[e][0].posB[0] = (int16_t)((cx + STREAK_HALF_WIDTH) * 64.0f);
        g_streak_ribbon[e][0].posB[1] = (int16_t)(ENGINE_OFFSET_Y * 64.0f);
        g_streak_ribbon[e][0].posB[2] = (int16_t)(ENGINE_OFFSET_Z * 64.0f);
        /* Rear verts: trail back along -Z (rewritten per frame). */
        g_streak_ribbon[e][1].posA[0] = (int16_t)((cx - STREAK_HALF_WIDTH) * 64.0f);
        g_streak_ribbon[e][1].posA[1] = (int16_t)(ENGINE_OFFSET_Y * 64.0f);
        g_streak_ribbon[e][1].posA[2] = (int16_t)((ENGINE_OFFSET_Z - STREAK_BASE_LEN) * 64.0f);
        g_streak_ribbon[e][1].posB[0] = (int16_t)((cx + STREAK_HALF_WIDTH) * 64.0f);
        g_streak_ribbon[e][1].posB[1] = (int16_t)(ENGINE_OFFSET_Y * 64.0f);
        g_streak_ribbon[e][1].posB[2] = (int16_t)((ENGINE_OFFSET_Z - STREAK_BASE_LEN) * 64.0f);
        g_streak_ribbon[e][0].normA = np; g_streak_ribbon[e][0].normB = np;
        g_streak_ribbon[e][1].normA = np; g_streak_ribbon[e][1].normB = np;
    }
}

/* Rewrite the rear pair's z to the requested trail length, and set the
 * vertex colour. Length is in model units (we multiply by 64 to match
 * the make_streak_ribbon scaling into the RSP's s16.16 vertex space). */
static void draw_streak(int engine_idx, float length, uint32_t rgba)
{
    T3DVertPacked *r = g_streak_ribbon[engine_idx];
    int16_t rear_z = (int16_t)((ENGINE_OFFSET_Z - length) * 64.0f);
    r[1].posA[2] = rear_z;
    r[1].posB[2] = rear_z;
    r[0].rgbaA = rgba; r[0].rgbaB = rgba;
    r[1].rgbaA = rgba; r[1].rgbaB = rgba;

    t3d_vert_load(r, 0, 4);
    t3d_tri_draw(0, 1, 2);
    t3d_tri_draw(2, 1, 3);
    t3d_tri_sync();
}

/* ── Star field (30 hand-picked positions) ──────────────────────────── */

static const fm_vec3_t g_stars[STAR_COUNT] = {
    {{  180,  60,  -90 }}, {{  -60, 100,  200 }}, {{  220, -40,  150 }},
    {{ -200,  20,  -30 }}, {{   40,  90, -250 }}, {{  -80, -20,  320 }},
    {{  160, -80,  -60 }}, {{ -150,  60,   80 }}, {{  100, 130, -180 }},
    {{  -20, -50,  -90 }}, {{  240,  40,   60 }}, {{ -180, -90, -100 }},
    {{   80, 110,  270 }}, {{ -100,  30, -160 }}, {{  140, -60,  220 }},
    {{  -40, 140,  -40 }}, {{  200,  80, -220 }}, {{ -240,  10,  170 }},
    {{  100, -30,  120 }}, {{ -120,  90,  -50 }}, {{   20, -90, -260 }},
    {{  170,  20, -120 }}, {{ -190, -40,  110 }}, {{  260,  70,  -30 }},
    {{ -100,  60,  240 }}, {{  120, 100,  100 }}, {{  -40, 120,  -10 }},
    {{  210,  30, -180 }}, {{ -160, -50,  -80 }}, {{   50,  10,  310 }},
};

static T3DVertPacked *g_cube_star;

/* Same 12-tri cube the other demos use; half-extent 1.0, white. */
static T3DVertPacked *make_star_cube(void)
{
    T3DVertPacked *v = malloc_uncached(sizeof(T3DVertPacked) * 4);
    const int16_t s = 1;
    const int16_t c[8][3] = {
        {-s,-s,-s},{ s,-s,-s},{ s, s,-s},{-s, s,-s},
        {-s,-s, s},{ s,-s, s},{ s, s, s},{-s, s, s},
    };
    for (int i = 0; i < 8; i += 2) {
        fm_vec3_t na = {{ (float)c[i][0], (float)c[i][1], (float)c[i][2] }};
        fm_vec3_t nb = {{ (float)c[i+1][0], (float)c[i+1][1], (float)c[i+1][2] }};
        fm_vec3_norm(&na, &na);
        fm_vec3_norm(&nb, &nb);
        v[i / 2] = (T3DVertPacked){
            .posA = { c[i][0], c[i][1], c[i][2] }, .rgbaA = color_to_packed32(RGBA32(200, 210, 255, 255)),
            .normA = t3d_vert_pack_normal(&na),
            .posB = { c[i+1][0], c[i+1][1], c[i+1][2] }, .rgbaB = color_to_packed32(RGBA32(200, 210, 255, 255)),
            .normB = t3d_vert_pack_normal(&nb),
        };
    }
    return v;
}

/* Cube triangle list — 12 tris for a single winding. Stars don't care
 * about backface culling for a 1-unit cube in deep space, so a single
 * 12-tri draw per star is fine. */
static const uint8_t CUBE_TRIS[12][3] = {
    {0,1,2},{2,3,0}, {4,6,5},{6,4,7},
    {0,4,5},{5,1,0}, {1,5,6},{6,2,1},
    {2,6,7},{7,3,2}, {3,7,4},{4,0,3},
};

static void draw_stars(void)
{
    for (int i = 0; i < STAR_COUNT; i++) {
        KilnTransform t;
        kiln_transform_init(&t);
        t.pos = g_stars[i];
        t.scale = (fm_vec3_t){{ 1.0f, 1.0f, 1.0f }};
        kiln_transform_push(&t);
        t3d_vert_load(g_cube_star, 0, 8);
        for (int j = 0; j < 12; j++) {
            t3d_tri_draw(CUBE_TRIS[j][0], CUBE_TRIS[j][1], CUBE_TRIS[j][2]);
        }
        t3d_tri_sync();
        kiln_transform_pop();
        kiln_transform_free(&t);
    }
}

/* ── Streak colour: premultiplied amber, alpha-modulated ──────────────
 *
 * The expression is:
 *   breathing = 0.7 + 0.3 * fm_sinf(total_t * 1.5)        // [0.4, 1.0]
 *   pulse_peak = 0.5 * (1 / (1 + pulse_t * 8))            // [0.5 -> ~0]
 *   modulation = breathing + pulse_peak
 * The streak is brighter on entry to a new mode and breathes slowly
 * while it lasts. The output is *premultiplied* — RGB held in the warm
 * amber band, A scaled — so on a dark background the streaks read as
 * additive glow without an rdpq_mode_blender() call.
 */
static void streak_color(float modulation, uint32_t *out)
{
    if (modulation < 0.0f) modulation = 0.0f;
    if (modulation > 1.5f) modulation = 1.5f;
    uint8_t a = (uint8_t)(STREAK_BASE_A * modulation);
    if (a > 255) a = 255;
    *out = color_to_packed32(RGBA32(255, 176, 72, a));
}

/* ── Per-mode state computation ─────────────────────────────────────── */

typedef struct {
    /* Ship world transform. */
    fm_vec3_t ship_pos;
    float     ship_yaw;
    float     ship_pitch;
    float     ship_roll;
    /* Camera. */
    fm_vec3_t cam_pos;
    fm_vec3_t cam_target;
    /* Streak params (after breathing + pulse_peak modulation applied). */
    float     streak_length;
    float     streak_modulation;
} ModeState;

static void compute_mode_state(ModeState *s)
{
    /* Defaults that all three modes share. */
    s->ship_yaw   = 0.0f;
    s->ship_pitch = 0.0f;
    s->ship_roll  = 0.0f;
    s->ship_pos   = (fm_vec3_t){{ 0.0f, 0.0f, 0.0f }};
    s->cam_target = (fm_vec3_t){{ 0.0f, 0.0f, 0.0f }};

    /* Per-mode camera + ship. */
    switch (mode) {
    case MODE_PARKED: {
        /* Ship yaws on the spot. Camera does a slow 360° orbit at a
         * moderate height, looking at the ship. */
        s->ship_yaw = total_t * 0.4f;
        float orbit_t = total_t * 0.35f;
        s->cam_pos = (fm_vec3_t){{
            140.0f * fm_cosf(orbit_t),
            55.0f,
            140.0f * fm_sinf(orbit_t)
        }};
        s->cam_target = s->ship_pos;
        s->streak_length = 0.8f;     /* parked, just an idle wisp */
        s->streak_modulation = 0.5f; /* dimmer than flying */
        break;
    }
    case MODE_BANKED: {
        /* Figure-8: x = 90*sin(2t), z = 60*sin(t), y bobs slightly. */
        float t = total_t * 0.9f;
        s->ship_pos = (fm_vec3_t){{
            90.0f * fm_sinf(2.0f * t),
            8.0f * fm_sinf(t * 1.3f),
            60.0f * fm_sinf(t)
        }};
        /* Heading: tangent to the curve, derived analytically. The
         * figure-8 has velocity (-180 cos(2t), 0.4 cos(1.3t), 60 cos(t))
         * — we only use the XZ components for yaw; the y bob is purely
         * positional and doesn't enter the heading. */
        float vx = -180.0f * fm_cosf(2.0f * t);
        float vz =   60.0f * fm_cosf(t);
        s->ship_yaw   = fm_atan2f(vx, vz);
        s->ship_roll  = -0.5f * (vx * fm_sinf(s->ship_yaw) - vz * fm_cosf(s->ship_yaw)) * 0.02f;
        s->ship_pitch =  0.08f * fm_sinf(t);
        /* Camera trails behind, slightly elevated. */
        s->cam_pos = (fm_vec3_t){{
            s->ship_pos.v[0] - 80.0f * fm_sinf(s->ship_yaw),
            30.0f,
            s->ship_pos.v[2] - 80.0f * fm_cosf(s->ship_yaw)
        }};
        s->cam_target = s->ship_pos;
        s->streak_length = 1.4f;
        s->streak_modulation = 1.0f;
        break;
    }
    case MODE_CINEMATIC: {
        /* Ship pulls away from camera along a curved arc. Camera holds
         * position, ship does the work. The arc is (sin(t), 0.5*sin(0.7t),
         * -t * 40) — a slow climb and retreat. */
        float t = time_in_mode;
        s->ship_pos = (fm_vec3_t){{
            40.0f * fm_sinf(t * 0.8f),
            10.0f + 8.0f * fm_sinf(t * 0.5f),
            -40.0f * t
        }};
        /* Ship yaw tracks its travel direction (the +Z component is
         * negative, so we negate to get the right forward angle). */
        float vx =  32.0f * fm_cosf(t * 0.8f);
        float vz = -40.0f;
        s->ship_yaw   = fm_atan2f(vx, -vz);   /* -vz because model is nose-at -Z */
        s->ship_roll  =  0.25f * fm_sinf(t * 0.8f);
        s->ship_pitch =  0.12f * fm_sinf(t * 0.5f);
        /* Camera: offset, looking at the ship's mid-point along the arc. */
        s->cam_pos = (fm_vec3_t){{ 60.0f, 25.0f, 40.0f }};
        s->cam_target = s->ship_pos;
        s->streak_length = 1.8f;
        s->streak_modulation = 1.2f;
        break;
    }
    }

    /* Apply breathing + pulse on top of the per-mode base modulation. */
    float breathing  = 0.7f + 0.3f * fm_sinf(total_t * 1.5f);
    float pulse_peak = 0.5f * (1.0f / (1.0f + pulse_t * 8.0f));
    s->streak_modulation *= (breathing + pulse_peak);
}

/* ── HUD: title screen ───────────────────────────────────────────────── */

static void draw_title_screen(void)
{
    /* Full-screen dark backdrop. */
    kiln_gui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA32(8, 10, 24, 255));
    /* Inner border panel. */
    kiln_gui_panel(4, 4, SCREEN_W - 8, SCREEN_H - 8,
                  RGBA32(0, 0, 0, 0), RGBA32(0, 245, 212, 255));

    const color_t white = RGBA32(232, 232, 240, 255);
    const color_t cyan  = RGBA32(0, 245, 212, 255);
    const color_t dim   = RGBA32(160, 170, 200, 255);

    kiln_gui_text(24, 70,  cyan,  "Kiln");
    kiln_gui_text(24, 90,  cyan,  "INTERCEPTOR DEMO");
    kiln_gui_text(24, 120, white, "ALH477");
    kiln_gui_text(24, 140, white, "not sponsored or endorsed by ModRetro");
    kiln_gui_text(24, 160, white, "MIT licensed engine");
    kiln_gui_text(24, 180, white, "a mix of the Ocarina of Time");
    kiln_gui_text(24, 192, white, "and IdTech4 engine for Kiln");
    kiln_gui_text(24, 210, dim,   "engine processes your assets + code");
    kiln_gui_text(24, 222, dim,   "like IdTech does");
    kiln_gui_text(24, SCREEN_H - 18, cyan, "press any button");
}

/* ── HUD: in-flight ──────────────────────────────────────────────────── */

static void draw_hud(int fps_int_x10)
{
    const color_t cyan  = RGBA32(0, 245, 212, 255);
    const color_t white = RGBA32(232, 232, 240, 255);
    const color_t dim   = RGBA32(160, 170, 200, 255);
    const color_t violet = RGBA32(232, 84, 138, 255);

    /* Top-left: lineage block. */
    kiln_gui_panel(8, 8, 192, 64, RGBA32(10, 10, 24, 200), cyan);
    kiln_gui_text(14, 18, cyan,  "KILN ENGINE");
    kiln_gui_text(14, 30, white, "OoT camera + idTech4 lighting");
    kiln_gui_text(14, 42, white, "Tiny3D on RSP");
    kiln_gui_text(14, 58, dim,   "fps %2d.%d", fps_int_x10 / 10, fps_int_x10 % 10);

    /* Top-right: mode label. */
    kiln_gui_panel(SCREEN_W - 104, 8, 96, 40, RGBA32(10, 10, 24, 200), cyan);
    kiln_gui_text(SCREEN_W - 98, 18, cyan,  "MODE %s", MODE_NAME[mode]);
    kiln_gui_text(SCREEN_W - 98, 32, white, "t %4.1f/%4.1f", time_in_mode, MODE_PERIOD[mode]);

    /* Bottom: credit strip. */
    kiln_gui_panel(8, SCREEN_H - 32, SCREEN_W - 16, 24, RGBA32(10, 10, 24, 200), violet);
    kiln_gui_text(14, SCREEN_H - 22, white, "ALH477  *  MIT  *  not sponsored by ModRetro");
    kiln_gui_text(14, SCREEN_H - 10, dim,   "engine processes your assets + code like IdTech does");
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(void)
{
    /* ── 1. Engine + display + DFS. */
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);
    asset_init_compression(2); /* matches mkasset -c 2 (see nix/assets.nix) */

    /* ── 2. Audio (deferred play until title exit). */
    kiln_audio_init(KILN_AUDIO_DEFAULT);
    g_blip  = kiln_sfx_load("rom:/sfx/blip.wav64");
    g_music = kiln_music_load("rom:/music/test.xm64");

    /* ── 3. Model + scene. */
    g_ship = t3d_model_load("rom:/models/interceptor.t3dm");
    kiln_transform_init(&g_ship_xform);
    make_streak_ribbon();
    g_cube_star = make_star_cube();

    kiln_scene_init(&g_scene);
    g_scene.far_z = 800.0f; /* default 400, bumped to fit the 600-unit star shell */

    /* ── 4. Frame state. */
    uint32_t frames = 0;
    int    fps_int_x10 = 0;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        joypad_poll();
        joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);

        /* ── Title screen branch. */
        if (in_title) {
            title_t += DT;
            bool any_input = (in.btn.a || in.btn.start || in.btn.b ||
                              in.stick_x != 0 || in.stick_y != 0 ||
                              in.cstick_x != 0 || in.cstick_y != 0);
            if (title_t >= 3.0f || any_input) {
                in_title = false;
                if (g_music >= 0) {
                    kiln_music_play(g_music);
                    kiln_music_set_volume(g_music, 0.7f);
                }
                /* Arm the first-mode blip. */
                blip_armed = true;
            }

            kiln_frame_begin();
            kiln_gui_begin();
            draw_title_screen();
            kiln_gui_end();
            kiln_frame_end();
            kiln_audio_update();
            continue;
        }

        /* ── In-flight timing. */
        time_in_mode += DT;
        total_t += DT;
        pulse_t += DT;
        blip_beat_t -= DT;

        if (time_in_mode >= MODE_PERIOD[mode]) {
            time_in_mode -= MODE_PERIOD[mode];
            mode = (mode + 1) % MODE_COUNT;
            pulse_t = 0.0f;
            blip_armed = true;
            blip_cine_armed = (mode == MODE_CINEMATIC);
        }

        /* ── One blip on mode entry, one extra on CINEMATIC at 1s. */
        if (blip_armed && blip_beat_t <= 0.0f) {
            kiln_sfx_play(g_blip, -1, 1);
            blip_beat_t = 0.20f;
            blip_armed = false;
        }
        if (blip_cine_armed && mode == MODE_CINEMATIC &&
            time_in_mode >= 1.0f && time_in_mode < 1.0f + DT) {
            kiln_sfx_play(g_blip, -1, 1);
            blip_cine_armed = false;
        }

        /* ── Compute ship + camera + streak for this mode. */
        ModeState ms;
        compute_mode_state(&ms);

        /* ── FPS every 30 frames. */
        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            float fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
            fps_int_x10 = (int)(fps * 10.0f);
        }

        /* ── 3D pass. */
        kiln_frame_begin();
        kiln_scene_begin(&g_scene);

        /* Update + push the ship transform. Streaks ride along — their
         * engine-mouth offsets are baked into the verts, so they share
         * the ship's world transform via a single push. */
        g_ship_xform.pos   = ms.ship_pos;
        g_ship_xform.scale = (fm_vec3_t){{ 1.0f, 1.0f, 1.0f }};
        g_ship_xform.rot_axis = (fm_vec3_t){{ 0.0f, 1.0f, 0.0f }};
        g_ship_xform.rot_angle = ms.ship_yaw;
        kiln_transform_push(&g_ship_xform);
        t3d_model_draw(g_ship);
        /* Two engine streaks inside the same push so they inherit
         * the ship's rotation/position. No nested xform needed —
         * the ribbon's verts already encode NACELLE_X and
         * ENGINE_OFFSET_Y/Z in model space. */
        uint32_t streak_rgba;
        streak_color(ms.streak_modulation, &streak_rgba);
        for (int i = 0; i < 2; i++) {
            draw_streak(i, ms.streak_length, streak_rgba);
        }
        kiln_transform_pop();

        /* Stars (drawn after the ship so they sit behind it visually,
         * though Z-buffering handles that in practice). */
        draw_stars();

        /* ── 2D HUD. */
        kiln_gui_begin();
        draw_hud(fps_int_x10);
        kiln_gui_end();

        kiln_frame_end();
        kiln_audio_update();
    }
}
