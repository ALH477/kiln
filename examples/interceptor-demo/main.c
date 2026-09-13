// SPDX-License-Identifier: MIT
//
// Interceptor demo — the hero prop (tools/blender/interceptor.py) flying
// through a starfield, lit and fogged, with engine streaks, a HUD, XM music
// and a blip on every cut.
//
//   models/interceptor.t3dm   the ship, rigid and vertex-coloured
//   sfx/blip.wav64            the cut / boost blip
//   music/test.xm64           the soundtrack (silent on the host build)
//
// ── The story ───────────────────────────────────────────────────────────
// A 3 s credit title over the parked ship, then a showcase that loops:
//
//   PARKED ORBIT   the ship idles in place, the camera circles it at 620
//   BANKED FIG-8   a figure-8 at ~700 u/s, banking into every turn, chased
//   CINEMATIC      a fixed camera watches it boost away into the haze
//
// Touch the stick or a button and the showcase hands you the ship where it
// is (FREE FLIGHT: stick steers, A boosts, B brakes). Five idle seconds and
// the showcase takes it back.
//
// ── Scale, because the old demo got it wrong ───────────────────────────
// gltf_to_t3d bakes x64 into the vertices, so the ship is ~365 units nose to
// tail and ~345 across. The camera used to orbit at radius 140 and trail 80
// units behind it: inside the hull. Everything below is sized against 365.
//
// Model axes (interceptor.py): nose at -Z, up +Y, right wing +X. So the
// exhaust mouths are at Blender (±0.80, -2.55, -0.06) -> engine
// (±51, -4, +163): the streaks trail along +Z from there. (They used to be
// placed at Y = -163, hanging a ship-length below the hull.)
//
// ── Attitude ───────────────────────────────────────────────────────────
// KilnTransform is one axis-angle, which can hold yaw OR a bank but not both,
// and the old demo computed pitch and roll and then dropped them. The ship
// matrix is composed here as Ry(yaw) * Rx(pitch) * Rz(roll) and pushed
// directly; the streaks are pushed INSIDE it, so they inherit the attitude.
//
// ── Stars ──────────────────────────────────────────────────────────────
// Three layers of unlit tetrahedra, so the parallax reads:
//   sky   160 stars on a 2000-unit shell that travels with the camera — they
//         never move relative to it, like a skybox;
//   far    72 stars fixed in the world 2600-4200 out, fading into the fog;
//   dust   48 motes wrapped into a 1400-unit box round the camera, which
//         stream past as it moves. Rewritten every frame, so they ping-pong
//         between two buffers: the RSP reads a vertex buffer asynchronously.
// Each star is sized for ~3 px at its distance; the old 2-unit cubes at 100-
// 300 units were a pixel or less.
//
// Jump ROMs (nix/demos/interceptor-demo.nix): TTL holds the title, ORB / FIG /
// CIN latch one showcase mode, FLY plays a steering tape in free flight.

#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmodel.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_audio.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_prim.h>

#include <malloc.h>

enum { JUMP_NONE, JUMP_TTL, JUMP_ORB, JUMP_FIG, JUMP_CIN, JUMP_FLY };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W 320
#define SCREEN_H 240
#define DT (1.0f / 60.0f)
#define PI 3.14159265f

#define TITLE_SECONDS 3.0f
#define MODE_SECONDS  8.0f
#define IDLE_FRAMES   300   /* 5 s of no input in free flight -> showcase */

enum { MODE_PARKED, MODE_BANKED, MODE_CINEMATIC, MODE_FREE, MODE_COUNT };
static const char *const MODE_NAME[MODE_COUNT] = {
    "PARKED ORBIT", "BANKED FIG-8", "CINEMATIC", "FREE FLIGHT",
};

/* ── Free-flight tape (the FLY jump ROM) ─────────────────────────────────
 * Raw stick counts, ±85 full tilt. S-turns with a climb and a boost. */
static const KilnInputKey FLY_KEYS[] = {
    { .frame =   0, .sx =  55, .sy = -20 },
    { .frame = 150, .sx = -60, .sy =  25 },
    { .frame = 300, .sx =  10, .sy = -45, .buttons = KILN_BTN_A },
    { .frame = 390, .sx =  65 },
    { .frame = 520, .sx = -35, .sy =  30 },
    { .frame = 640 },
};
static const KilnInputTape FLY_TAPE = { FLY_KEYS, 6, 0 };

/* ── Tiny deterministic RNG for star placement ─────────────────────────── */
static uint32_t g_seed = 0x1D2E3F41u;
static float frand(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return (float)(g_seed >> 8) / 16777216.0f;
}

static fm_vec3_t rand_dir(void)
{
    for (;;) {
        fm_vec3_t d = {{ frand() * 2 - 1, frand() * 2 - 1, frand() * 2 - 1 }};
        float l2 = d.v[0] * d.v[0] + d.v[1] * d.v[1] + d.v[2] * d.v[2];
        if (l2 > 0.05f && l2 <= 1.0f) {
            fm_vec3_norm(&d, &d);
            return d;
        }
    }
}

static float wrap_pi(float a)
{
    while (a >  PI) a -= 2 * PI;
    while (a < -PI) a += 2 * PI;
    return a;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ── Tetrahedra: 4 vertices = 2 packed entries, 4 triangles ───────────── */
#define TET_BATCH 17   /* 68 vertices per t3d_vert_load */

static void tet_put(T3DVertPacked *s, fm_vec3_t c, float h, uint32_t rgba)
{
    int16_t x = (int16_t)c.v[0], y = (int16_t)c.v[1], z = (int16_t)c.v[2];
    int16_t k = (int16_t)(h < 2.0f ? 2.0f : h);
    static const fm_vec3_t up = {{ 0, 1, 0 }};
    uint16_t n = t3d_vert_pack_normal((fm_vec3_t *)&up);
    s[0] = (T3DVertPacked){ .posA = { x + k, y + k, z + k }, .rgbaA = rgba, .normA = n,
                            .posB = { x + k, y - k, z - k }, .rgbaB = rgba, .normB = n };
    s[1] = (T3DVertPacked){ .posA = { x - k, y + k, z - k }, .rgbaA = rgba, .normA = n,
                            .posB = { x - k, y - k, z + k }, .rgbaB = rgba, .normB = n };
}

static void draw_tets(const T3DVertPacked *v, int tets)
{
    for (int done = 0; done < tets;) {
        int b = tets - done;
        if (b > TET_BATCH) b = TET_BATCH;
        t3d_vert_load(v + done * 2, 0, (uint32_t)(b * 4));
        for (int i = 0; i < b; i++) {
            const uint32_t o = (uint32_t)(i * 4);
            t3d_tri_draw(o, o + 1, o + 2);
            t3d_tri_draw(o, o + 3, o + 1);
            t3d_tri_draw(o, o + 2, o + 3);
            t3d_tri_draw(o + 1, o + 3, o + 2);
        }
        t3d_tri_sync();
        done += b;
    }
}

static uint32_t star_colour(float k)
{
    /* Three families: blue-white, white, warm. */
    const float pick = frand();
    uint8_t r, g, b;
    if (pick < 0.45f)      { r = 190; g = 210; b = 255; }
    else if (pick < 0.85f) { r = 245; g = 245; b = 240; }
    else                   { r = 255; g = 200; b = 150; }
    return kiln_prim_rgba((uint8_t)(r * k), (uint8_t)(g * k), (uint8_t)(b * k));
}

#define SKY_N  160
#define FAR_N   72
#define DUST_N  48
#define SKY_R  2000.0f
#define DUST_BOX 1400.0f

static T3DVertPacked *g_sky;           /* camera-relative, static */
static T3DVertPacked *g_far;           /* world, static */
static T3DVertPacked *g_dust[2];       /* camera-relative, ping-pong */
static fm_vec3_t      g_dust_base[DUST_N];
static uint32_t       g_dust_rgba[DUST_N];

static void build_stars(void)
{
    g_sky = malloc_uncached(sizeof(T3DVertPacked) * SKY_N * 2);
    for (int i = 0; i < SKY_N; i++) {
        fm_vec3_t d = rand_dir();
        const float bright = 0.55f + 0.45f * frand();
        const float h = SKY_R * (frand() < 0.12f ? 0.0075f : 0.0048f);
        tet_put(g_sky + i * 2, (fm_vec3_t){{ d.v[0] * SKY_R, d.v[1] * SKY_R, d.v[2] * SKY_R }},
                h, star_colour(bright));
    }
    g_far = malloc_uncached(sizeof(T3DVertPacked) * FAR_N * 2);
    for (int i = 0; i < FAR_N; i++) {
        fm_vec3_t d = rand_dir();
        const float r = 2600.0f + 1600.0f * frand();
        tet_put(g_far + i * 2, (fm_vec3_t){{ d.v[0] * r, d.v[1] * r, d.v[2] * r }},
                r * 0.0055f, star_colour(0.8f + 0.2f * frand()));
    }
    for (int i = 0; i < DUST_N; i++) {
        g_dust_base[i] = (fm_vec3_t){{ frand() * DUST_BOX, frand() * DUST_BOX, frand() * DUST_BOX }};
        const uint8_t k = (uint8_t)(90 + 70 * frand());
        g_dust_rgba[i] = kiln_prim_rgba(k, (uint8_t)(k + 10), (uint8_t)(k + 30));
    }
    g_dust[0] = malloc_uncached(sizeof(T3DVertPacked) * DUST_N * 2);
    g_dust[1] = malloc_uncached(sizeof(T3DVertPacked) * DUST_N * 2);
}

/* Rewrite one dust buffer: each mote wrapped into a box centred on the camera,
 * written camera-relative (so the positions stay inside int16 wherever free
 * flight goes). A mote within 60 units of the eye collapses to a point rather
 * than filling the screen. */
static void fill_dust(T3DVertPacked *buf, fm_vec3_t cam)
{
    const float half = DUST_BOX * 0.5f;
    for (int i = 0; i < DUST_N; i++) {
        fm_vec3_t p;
        for (int a = 0; a < 3; a++) {
            float r = g_dust_base[i].v[a] - cam.v[a];
            r -= DUST_BOX * (float)(int)(r / DUST_BOX);
            if (r < -half) r += DUST_BOX;
            if (r >= half) r -= DUST_BOX;
            p.v[a] = r;
        }
        const float d2 = p.v[0] * p.v[0] + p.v[1] * p.v[1] + p.v[2] * p.v[2];
        tet_put(buf + i * 2, p, d2 < 62500.0f ? 0.0f : 2.0f, g_dust_rgba[i]);
    }
}

/* ── Engine streaks ──────────────────────────────────────────────────────
 * Two crossed ribbons per engine (a flat ribbon vanishes edge-on), 256 units
 * long in their own space and scaled to length by a transform, so the vertex
 * data never changes. Brightness is one of STREAK_LEVELS prebuilt buffers for
 * the same reason. Unlit; hot at the mouth, fading to ember at the tail — on
 * a near-black sky that reads as glow with no blender. */
#define STREAK_LEVELS 6
#define STREAK_LEN_MODEL 256.0f
static const fm_vec3_t ENGINE_MOUTH[2] = { {{ -51.0f, -4.0f, 163.0f }}, {{ 51.0f, -4.0f, 163.0f }} };
static T3DVertPacked *g_streak[STREAK_LEVELS];

static void build_streaks(void)
{
    static const fm_vec3_t n = {{ 0, 1, 0 }};
    const uint16_t np = t3d_vert_pack_normal((fm_vec3_t *)&n);
    const int16_t W0 = 16, W1 = 2, L = (int16_t)STREAK_LEN_MODEL;
    for (int lv = 0; lv < STREAK_LEVELS; lv++) {
        const float k = 0.45f + 0.55f * (float)lv / (STREAK_LEVELS - 1);
        const uint32_t hot = kiln_prim_rgba((uint8_t)(255 * k), (uint8_t)(190 * k), (uint8_t)(90 * k));
        const uint32_t tail = kiln_prim_rgba((uint8_t)(90 * k), (uint8_t)(30 * k), (uint8_t)(12 * k));
        T3DVertPacked *v = malloc_uncached(sizeof(T3DVertPacked) * 4);
        /* Horizontal ribbon: front-left, back-left, back-right, front-right. */
        v[0] = (T3DVertPacked){ .posA = { -W0, 0, 0 }, .rgbaA = hot,  .normA = np,
                                .posB = { -W1, 0, L }, .rgbaB = tail, .normB = np };
        v[1] = (T3DVertPacked){ .posA = {  W1, 0, L }, .rgbaA = tail, .normA = np,
                                .posB = {  W0, 0, 0 }, .rgbaB = hot,  .normB = np };
        /* Vertical ribbon. */
        v[2] = (T3DVertPacked){ .posA = { 0, -W0, 0 }, .rgbaA = hot,  .normA = np,
                                .posB = { 0, -W1, L }, .rgbaB = tail, .normB = np };
        v[3] = (T3DVertPacked){ .posA = { 0,  W1, L }, .rgbaA = tail, .normA = np,
                                .posB = { 0,  W0, 0 }, .rgbaB = hot,  .normB = np };
        g_streak[lv] = v;
    }
}

static void draw_streak(int level)
{
    t3d_vert_load(g_streak[level], 0, 8);
    t3d_tri_draw(0, 1, 2); t3d_tri_draw(0, 2, 3);
    t3d_tri_draw(4, 5, 6); t3d_tri_draw(4, 6, 7);
    t3d_tri_sync();
}

/* ── Ship attitude matrix: Ry(yaw) * Rx(pitch) * Rz(roll), then translate ─ */
static void ship_matrix(fm_mat4_t *m, fm_vec3_t pos, float yaw, float pitch, float roll)
{
    const float cy = fm_cosf(yaw),   sy = fm_sinf(yaw);
    const float cp = fm_cosf(pitch), sp = fm_sinf(pitch);
    const float cr = fm_cosf(roll),  sr = fm_sinf(roll);
    const float Ry[3][3] = { { cy, 0, sy }, { 0, 1, 0 }, { -sy, 0, cy } };
    const float Rx[3][3] = { { 1, 0, 0 }, { 0, cp, -sp }, { 0, sp, cp } };
    const float Rz[3][3] = { { cr, -sr, 0 }, { sr, cr, 0 }, { 0, 0, 1 } };
    float a[3][3], r[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            a[i][j] = Ry[i][0] * Rx[0][j] + Ry[i][1] * Rx[1][j] + Ry[i][2] * Rx[2][j];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r[i][j] = a[i][0] * Rz[0][j] + a[i][1] * Rz[1][j] + a[i][2] * Rz[2][j];
    /* fm_mat4_t is column-major (m[col][row]); translation lives in m[3]. */
    *m = (fm_mat4_t){0};
    for (int c = 0; c < 3; c++)
        for (int rr = 0; rr < 3; rr++)
            m->m[c][rr] = r[rr][c];
    m->m[3][0] = pos.v[0];
    m->m[3][1] = pos.v[1];
    m->m[3][2] = pos.v[2];
    m->m[3][3] = 1.0f;
}

/* Nose direction for a yaw and pitch: Ry * Rx * (0, 0, -1). */
static fm_vec3_t nose_dir(float yaw, float pitch)
{
    const float cp = fm_cosf(pitch);
    return (fm_vec3_t){{ -fm_sinf(yaw) * cp, fm_sinf(pitch), -fm_cosf(yaw) * cp }};
}

/* Heading and climb that point the nose along a velocity. */
static void face_velocity(float vx, float vy, float vz, float *yaw, float *pitch)
{
    *yaw = fm_atan2f(-vx, -vz);
    const fm_vec3_t flat = {{ vx, 0.0f, vz }};
    *pitch = fm_atan2f(vy, fm_vec3_len(&flat));
}

/* ── Flight state ─────────────────────────────────────────────────────── */
typedef struct {
    fm_vec3_t pos;
    float yaw, pitch, roll;
    float speed;          /* units / second, for the HUD */
    float streak_len;     /* world units */
    float glow;           /* 0..1 */
} Ship;

typedef struct {
    fm_vec3_t pos, target;
    int snap;             /* next update jumps rather than eases */
} Cam;

static void cam_ease(Cam *c, fm_vec3_t pos, fm_vec3_t target, float pos_rate, float tgt_rate)
{
    if (c->snap) {
        c->pos = pos;
        c->target = target;
        c->snap = 0;
        return;
    }
    const float kp = clampf(pos_rate * DT, 0, 1), kt = clampf(tgt_rate * DT, 0, 1);
    for (int a = 0; a < 3; a++) {
        c->pos.v[a] += (pos.v[a] - c->pos.v[a]) * kp;
        c->target.v[a] += (target.v[a] - c->target.v[a]) * kt;
    }
}

/* A chase view: behind the nose, above it, looking a little ahead. */
static void cam_chase(Cam *c, const Ship *s, float dist, float height)
{
    const fm_vec3_t f = nose_dir(s->yaw, 0.0f);
    fm_vec3_t pos = {{ s->pos.v[0] - f.v[0] * dist, s->pos.v[1] + height, s->pos.v[2] - f.v[2] * dist }};
    fm_vec3_t n = nose_dir(s->yaw, s->pitch);
    fm_vec3_t tgt = {{ s->pos.v[0] + n.v[0] * 140, s->pos.v[1] + n.v[1] * 140, s->pos.v[2] + n.v[2] * 140 }};
    cam_ease(c, pos, tgt, 3.0f, 8.0f);
}

/* The parked pose, shared with the title. */
static void pose_parked(Ship *s, float t)
{
    s->pos = (fm_vec3_t){{ 0, 18.0f * fm_sinf(t * 0.8f), 0 }};
    s->yaw = 0.6f + t * 0.22f;
    s->pitch = 0.05f * fm_sinf(t * 0.7f);
    s->roll = 0.10f * fm_sinf(t * 0.9f);
    s->speed = 0;
    s->streak_len = 70.0f + 20.0f * fm_sinf(t * 3.0f);
    s->glow = 0.35f;
}

static void orbit(Cam *c, float angle, float radius, float height, fm_vec3_t look)
{
    fm_vec3_t pos = {{ look.v[0] + radius * fm_cosf(angle), look.v[1] + height,
                       look.v[2] + radius * fm_sinf(angle) }};
    c->snap = 1;
    cam_ease(c, pos, look, 0, 0);
}

/* Banking: roll eases toward a bank proportional to the yaw rate. Turning
 * right means yaw DECREASING (the nose swings toward +X from -Z), and a right
 * turn wants the right wing (+X) down, which is negative roll. */
static void bank(Ship *s, float prev_yaw, float gain)
{
    const float rate = wrap_pi(s->yaw - prev_yaw) / DT;
    const float want = clampf(rate * gain, -1.0f, 1.0f);
    s->roll += (want - s->roll) * clampf(4.0f * DT, 0, 1);
}

/* ── HUD ──────────────────────────────────────────────────────────────── */
static const color_t INK   = { 0xE8, 0xE8, 0xF0, 0xFF };
static const color_t DIM   = { 0x90, 0x98, 0xB8, 0xFF };
static const color_t TEAL  = { 0x00, 0xF5, 0xD4, 0xFF };
static const color_t AMBER = { 0xFF, 0xB0, 0x48, 0xFF };
static const color_t PANEL = { 0x0A, 0x0C, 0x1C, 0xFF };
static const color_t ROSE  = { 0xE8, 0x54, 0x8A, 0xFF };

static int deg(float rad) { return (int)(rad * 57.29578f); }

static void draw_title(float title_t, int held)
{
    kiln_gui_panel(8, 128, SCREEN_W - 16, 104, PANEL, TEAL);
    kiln_gui_text(16, 143, TEAL, "KILN  INTERCEPTOR");
    kiln_gui_text(16, 158, INK,  "by ALH477  -  MIT licensed engine");
    kiln_gui_text(16, 171, INK,  "not sponsored or endorsed by ModRetro");
    kiln_gui_text(16, 186, DIM,  "an Ocarina of Time + idTech 4 engine");
    kiln_gui_text(16, 199, DIM,  "your assets and code, processed like idTech");
    if (((int)(title_t * 2.0f)) % 2 == 0)
        kiln_gui_text(16, 220, AMBER, "press START");
    if (!held)
        kiln_gui_bar(112, 214, 192, 6, clampf(title_t / TITLE_SECONDS, 0, 1), TEAL, PANEL);
}

static void draw_hud(int mode, float time_in_mode, int showcase, const Ship *s, float fps,
                     int scripted)
{
    /* Top left: what this is and which mode. */
    kiln_gui_panel(8, 8, 150, 44, PANEL, TEAL);
    kiln_gui_text(14, 21, TEAL, "KILN INTERCEPTOR");
    kiln_gui_text(14, 34, INK, "%s", MODE_NAME[mode]);
    kiln_gui_text(124, 34, DIM, "%2d", (int)(fps + 0.5f));
    if (showcase)
        kiln_gui_bar(14, 41, 138, 4, clampf(time_in_mode / MODE_SECONDS, 0, 1), TEAL, PANEL);
    else
        kiln_gui_bar(14, 41, 138, 4, clampf(s->speed / 1100.0f, 0, 1), AMBER, PANEL);

    /* Top right: attitude. */
    int hdg = deg(s->yaw) % 360;
    if (hdg < 0) hdg += 360;
    kiln_gui_panel(SCREEN_W - 120, 8, 112, 44, PANEL, TEAL);
    kiln_gui_text(SCREEN_W - 114, 21, INK, "SPD %4d", (int)s->speed);
    kiln_gui_text(SCREEN_W - 114, 34, INK, "HDG %3d", hdg);
    kiln_gui_text(SCREEN_W - 114, 46, INK, "PIT%+3d ROL%+3d", deg(s->pitch), deg(s->roll));

    if (scripted) {
        kiln_gui_panel(SCREEN_W - 58, 58, 50, 16, ROSE, INK);
        kiln_gui_text(SCREEN_W - 49, 70, INK, "DEMO");
    }

    /* Bottom: credits and controls, both inside 300 px. */
    kiln_gui_panel(8, SCREEN_H - 36, SCREEN_W - 16, 28, PANEL, ROSE);
    kiln_gui_text(14, SCREEN_H - 24, INK, "ALH477 * MIT * not sponsored by ModRetro");
    kiln_gui_text(14, SCREEN_H - 12, DIM, showcase
                  ? "stick or button: take the ship"
                  : "stick steer  A boost  B brake  idle: tour");
}

/* ── main ─────────────────────────────────────────────────────────────── */
int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);
    kiln_input_init();

    kiln_audio_init(KILN_AUDIO_DEFAULT);
    const int sfx_blip = kiln_sfx_load("rom:/sfx/blip.wav64");
    const int music = kiln_music_load("rom:/music/test.xm64");

    T3DModel *ship_model = t3d_model_load("rom:/models/interceptor.t3dm");
    T3DMat4FP *ship_mtx = malloc_uncached(sizeof(T3DMat4FP));
    KilnTransform streak_xf[2], sky_xf;
    for (int e = 0; e < 2; e++) {
        kiln_transform_init(&streak_xf[e]);
        streak_xf[e].pos = ENGINE_MOUTH[e];
    }
    kiln_transform_init(&sky_xf);
    build_stars();
    build_streaks();

    KilnScene scene;
    kiln_scene_init(&scene);
    /* Fog 1100..2500 reads as ~2200..5000 of depth on the console: Tiny3D's
     * ucode ramps fog over about [2*near, 2*far] (offset -2*near on clip z),
     * where plat/host ramps over [near, far]. So the sky shell at 2000 stays
     * clear and the CINEMATIC pull-away fades into the haze in Ares; host
     * renders fog the far stars more heavily than the ROM does. */
    kiln_prim_stage(&scene, RGBA32(0x06, 0x08, 0x16, 0xFF), 1100.0f, 2500.0f);
    /* Both lights set here rather than inherited from the preset. A light
     * direction points TOWARD its source: Tiny3D's RSP lights a vertex by
     * +dot(normal, dir), so overhead is +y. Warm key high over the right
     * wing; cool rim from high behind-left, so the tail separates from the
     * sky. (plat/host lit by -dot until main fixed it, so judge these in Ares,
     * not in a host render.) */
    scene.light_dir = (fm_vec3_t){{ 0.35f, 0.80f, 0.45f }};
    fm_vec3_norm(&scene.light_dir, &scene.light_dir);
    scene.lights[0].dir = (fm_vec3_t){{ -0.50f, 0.30f, -0.80f }};
    fm_vec3_norm(&scene.lights[0].dir, &scene.lights[0].dir);
    scene.fov_deg = 60.0f;
    scene.near_z = 24.0f;
    scene.far_z = 5200.0f;

    int in_title = 1;
    float title_t = 0.0f;
    int mode = MODE_PARKED;
    float time_in_mode = 0.0f;
    int idle = 0;
    Ship ship = {0};
    Cam cam = { .snap = 1 };
    float pulse = 0.0f;       /* flash on a cut, decays */
    int cine_blip = 0;

    const int latched = (KILN_JUMP == JUMP_ORB || KILN_JUMP == JUMP_FIG || KILN_JUMP == JUMP_CIN);
    if (KILN_JUMP != JUMP_NONE && KILN_JUMP != JUMP_TTL) {
        in_title = 0;
        mode = KILN_JUMP == JUMP_FIG ? MODE_BANKED
             : KILN_JUMP == JUMP_CIN ? MODE_CINEMATIC
             : KILN_JUMP == JUMP_FLY ? MODE_FREE
             : MODE_PARKED;
        if (music >= 0) kiln_music_play(music);
    }
    if (KILN_JUMP == JUMP_FLY) {
        kiln_input_play(1, &FLY_TAPE);
        pose_parked(&ship, 0.0f);
        ship.speed = 450.0f;
    }

    uint32_t frames = 0, dust_flip = 0;
    float fps = 60.0f;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);
        const int touched = in->buttons != 0 || in->stick_x * in->stick_x + in->stick_y * in->stick_y > 0.04f;

        if (++frames % 30 == 0) {
            const uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        const float prev_yaw = ship.yaw;
        float streak_boost = 0.0f;

        if (in_title) {
            title_t += DT;
            pose_parked(&ship, title_t);
            orbit(&cam, 0.9f + title_t * 0.25f, 560.0f, 70.0f,
                  (fm_vec3_t){{ 0, -150.0f, 0 }});
            if (KILN_JUMP != JUMP_TTL && (title_t >= TITLE_SECONDS || in->edges)) {
                in_title = 0;
                mode = MODE_PARKED;
                time_in_mode = 0.0f;
                pulse = 1.0f;
                cam.snap = 1;
                if (sfx_blip >= 0) kiln_sfx_play(sfx_blip, -1, 1);
                if (music >= 0) {
                    kiln_music_play(music);
                    kiln_music_set_volume(music, 0.7f);
                }
            }
        } else if (mode == MODE_FREE) {
            /* ── Free flight ─────────────────────────────────────────── */
            idle = touched ? 0 : idle + 1;
            const float want = (in->buttons & KILN_BTN_A) ? 1100.0f
                             : (in->buttons & KILN_BTN_B) ? 180.0f : 450.0f;
            ship.speed += (want - ship.speed) * clampf(2.0f * DT, 0, 1);
            ship.yaw -= in->stick_x * 1.3f * DT;
            ship.pitch = clampf(ship.pitch - in->stick_y * 1.0f * DT, -1.1f, 1.1f);
            /* Level off gently when the stick is released. */
            if (in->stick_y * in->stick_y < 0.01f) ship.pitch *= 1.0f - 0.6f * DT;
            /* A leash: beyond 4500 units, steer home so positions stay well
             * inside the RSP's s16.16 range. */
            const float d2 = ship.pos.v[0] * ship.pos.v[0] + ship.pos.v[2] * ship.pos.v[2];
            if (d2 > 4500.0f * 4500.0f) {
                const float home = fm_atan2f(ship.pos.v[0], ship.pos.v[2]);
                ship.yaw += wrap_pi(home - ship.yaw) * 1.5f * DT;
            }
            if (ship.pos.v[1] > 1500.0f && ship.pitch > 0) ship.pitch -= 1.5f * DT;
            if (ship.pos.v[1] < -1500.0f && ship.pitch < 0) ship.pitch += 1.5f * DT;
            const fm_vec3_t n = nose_dir(ship.yaw, ship.pitch);
            for (int a = 0; a < 3; a++) ship.pos.v[a] += n.v[a] * ship.speed * DT;
            bank(&ship, prev_yaw, 0.9f);
            ship.streak_len = 120.0f + ship.speed * 0.4f;
            ship.glow = clampf(ship.speed / 1100.0f, 0.3f, 1.0f);
            cam_chase(&cam, &ship, 760.0f, 210.0f);

            if (KILN_JUMP == JUMP_NONE && idle >= IDLE_FRAMES) {
                mode = MODE_PARKED;
                time_in_mode = 0.0f;
                pulse = 1.0f;
                cam.snap = 1;
                if (sfx_blip >= 0) kiln_sfx_play(sfx_blip, -1, 1);
            }
        } else {
            /* ── Showcase ────────────────────────────────────────────── */
            time_in_mode += DT;
            if (time_in_mode >= MODE_SECONDS) {
                time_in_mode -= MODE_SECONDS;
                if (!latched) mode = (mode + 1) % MODE_FREE;
                pulse = 1.0f;
                cam.snap = 1;
                cine_blip = 1;
                if (sfx_blip >= 0) kiln_sfx_play(sfx_blip, -1, 1);
            }
            const float t = time_in_mode;

            if (mode == MODE_PARKED) {
                pose_parked(&ship, t);
                orbit(&cam, 0.4f + t * 0.45f, 620.0f, 150.0f + 50.0f * fm_sinf(t * 0.5f),
                      (fm_vec3_t){{ 0, 0, 0 }});
            } else if (mode == MODE_BANKED) {
                const float w = 0.4f, u = t * w;
                ship.pos = (fm_vec3_t){{ 900.0f * fm_sinf(2 * u), 80.0f * fm_sinf(1.3f * u),
                                         700.0f * fm_sinf(u) }};
                const float vx = 1800.0f * w * fm_cosf(2 * u);
                const float vy = 104.0f * w * fm_cosf(1.3f * u);
                const float vz = 700.0f * w * fm_cosf(u);
                face_velocity(vx, vy, vz, &ship.yaw, &ship.pitch);
                if (cam.snap) ship.roll = 0;
                else bank(&ship, prev_yaw, 1.1f);
                ship.speed = fm_vec3_len(&(fm_vec3_t){{ vx, vy, vz }});
                ship.streak_len = 260.0f;
                ship.glow = 0.75f;
                cam_chase(&cam, &ship, 720.0f, 220.0f);
            } else { /* MODE_CINEMATIC */
                ship.pos = (fm_vec3_t){{ 300.0f * fm_sinf(0.5f * t), 40.0f + 2.0f * t * t * 1.5f,
                                         -(80.0f * t + 45.0f * t * t) }};
                const float vx = 150.0f * fm_cosf(0.5f * t);
                const float vy = 6.0f * t;
                const float vz = -(80.0f + 90.0f * t);
                face_velocity(vx, vy, vz, &ship.yaw, &ship.pitch);
                if (cam.snap) ship.roll = 0;
                else bank(&ship, prev_yaw, 2.5f);
                ship.speed = fm_vec3_len(&(fm_vec3_t){{ vx, vy, vz }});
                const float surge = (t > 1.0f && t < 2.2f) ? 1.0f - (t - 1.0f) / 1.2f : 0.0f;
                streak_boost = surge;
                ship.streak_len = 320.0f + 420.0f * surge + 20.0f * t;
                ship.glow = clampf(0.8f + surge, 0, 1);
                if (cine_blip && t > 1.0f) {
                    cine_blip = 0;
                    pulse = 0.6f;
                    if (sfx_blip >= 0) kiln_sfx_play(sfx_blip, -1, 1);
                }
                /* A tripod that creeps back and up, easing onto the ship. */
                fm_vec3_t pos = {{ 380.0f + 10.0f * t, 110.0f + 12.0f * t, 560.0f + 30.0f * t }};
                fm_vec3_t tgt = ship.pos;
                if (cam.snap) cam_ease(&cam, pos, tgt, 0, 0);
                else cam_ease(&cam, pos, tgt, 6.0f, 5.0f);
            }

            if (KILN_JUMP == JUMP_NONE && touched) {
                mode = MODE_FREE;
                idle = 0;
                ship.speed = ship.speed < 300.0f ? 450.0f : ship.speed;
                ship.pitch = clampf(ship.pitch, -1.0f, 1.0f);
            }
        }

        pulse = pulse > 0 ? pulse - DT * 1.5f : 0;
        const float glow = clampf(ship.glow + 0.2f * fm_sinf((float)frames * 0.11f) * ship.glow +
                                  0.5f * pulse + streak_boost * 0.3f, 0, 1);
        const int level = (int)(glow * (STREAK_LEVELS - 1) + 0.5f);

        scene.cam_pos = cam.pos;
        scene.cam_target = cam.target;
        kiln_scene_update(&scene);

        /* Stars: the dust buffer NOT drawn last frame is the one rewritten. */
        dust_flip ^= 1;
        fill_dust(g_dust[dust_flip], cam.pos);

        /* ── 3D ──────────────────────────────────────────────────────── */
        kiln_frame_begin();
        kiln_scene_begin(&scene);

        t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT);
        sky_xf.pos = cam.pos;
        kiln_transform_push(&sky_xf);
        draw_tets(g_sky, SKY_N);
        draw_tets(g_dust[dust_flip], DUST_N);
        kiln_transform_pop();
        draw_tets(g_far, FAR_N);

        t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH);
        fm_mat4_t sm;
        ship_matrix(&sm, ship.pos, ship.yaw, ship.pitch, ship.roll);
        t3d_mat4_to_fixed(ship_mtx, &sm);
        t3d_matrix_push(ship_mtx);
        t3d_model_draw(ship_model);

        t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT);
        for (int e = 0; e < 2; e++) {
            const float wob = 1.0f + 0.06f * fm_sinf((float)frames * 0.37f + (float)e * 2.1f);
            streak_xf[e].scale = (fm_vec3_t){{ 1.0f, 1.0f, ship.streak_len * wob / STREAK_LEN_MODEL }};
            kiln_transform_push(&streak_xf[e]);
            draw_streak(level);
            kiln_transform_pop();
        }
        t3d_matrix_pop(1);

        /* ── 2D ──────────────────────────────────────────────────────── */
        kiln_gui_begin();
        if (in_title) draw_title(title_t, KILN_JUMP == JUMP_TTL);
        else draw_hud(mode, time_in_mode, mode != MODE_FREE, &ship, fps, kiln_input_scripted(1));
        kiln_gui_end();

        kiln_frame_end();
        kiln_audio_update();
    }
}
