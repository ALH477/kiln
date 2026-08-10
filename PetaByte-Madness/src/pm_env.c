// SPDX-License-Identifier: MPL-2.0
//
// pm_env.c — see pm_env.h.

#include "pm_env.h"

#include <libdragon.h>
#include <math.h>
#include <stdlib.h>
#include <t3d/t3d.h>
#include <t3d/t3dmodel.h>

#include <m64/m64_texanim.h>
#include <m64/m64_crater.h>

#include "pm_models.h"
#include "pm_fx.h"
#include "pm_sfx.h"
#include "pm_world_gen.h"

// ── Where the moon is ──────────────────────────────────────────────────
// The same azimuth and elevation tools/blender/pm_env.py bakes the moon
// disc and the sea's glitter lane at. Keeping the numbers in both places
// is deliberate: the alternative is a generated header for two floats, and
// a mismatch here is self-announcing — the moon path on the water points
// somewhere the moon is not.
//
// Blender authors +Z up; the engine is +Y up and gltf_to_t3d converts on
// export. These are written directly in ENGINE axes, which is why the
// elevation lands on Y and the azimuth sweeps X/Z.
#define MOON_AZ_COS   (-0.7880f)  // cos(218 deg)
#define MOON_AZ_SIN   (-0.6157f)  // sin(218 deg)
#define MOON_EL_COS    (0.8290f)  // cos(34 deg)
#define MOON_EL_SIN    (0.5592f)  // sin(34 deg)

// ── The sea's size ─────────────────────────────────────────────────────
// build_sea() authors in Blender units and the exporter multiplies by 64, so
// the model already arrives in world units and is drawn at scale 1. Its
// outer radius is 350 * 64 = 22,400 units: comfortably inside the flyover's
// 40,000 far plane, with the fog below finishing well before it.
//
// Scale 1 is not an accident, it is the whole reason the swell works. Drawn
// at any larger scale, one int16 vertex unit would be several world units
// and a believable swell would round to zero. See build_sea's docstring.
#define SEA_SCALE   1.0f

// The sky's radius is arbitrary — it is drawn depth-less and camera-centred,
// so it is a direction field. Big enough to be unambiguous, small enough to
// stay far away from the fixed-point limits.
#define SKY_SCALE   4000.0f

// ── Fog ────────────────────────────────────────────────────────────────
// Expressed as FRACTIONS of the shot's own far plane, not as absolute
// distances — fog is camera-relative and every shot frames something at a
// different range (the flyover's near orbit, the sub shot's far-off island,
// the beach's close interior). Tied to far_z, fog always finishes at or
// before geometry is clipped: anything past FOG_FAR_FRAC is already solid
// fog colour, so the far plane can be pulled in to meet it with the cut
// invisible, saving fill rate.
//
// FOG_FAR_FRAC used to be 1.0 — full fog exactly at the far plane, so the
// clip is never seen. That correctly hides the far plane, but it also means
// opacity ramps LINEARLY across the entire near..far span, and this island's
// low, close orbit (pm_demo.c's ORBIT_RADII) puts the eye-to-look-target
// distance at roughly 69% of far_z — most of a frame's visible foreground
// sat well under 50% fogged despite FOG_NEAR_FRAC starting the ramp almost
// at the lens. Starting the ramp early only clears a zone right at the
// camera; it does not control how FAST opacity climbs afterward, which is
// entirely (far-near). That gap between "fog is on" and "fog reads as
// thick" is why this looked too clear.
//
// FOG_FAR_FRAC now pulls full saturation to just past the mid-orbit terrain
// and short of the temple: nearby ground still visibly ramps (the depth cue
// survives) while the temple and the far shore arrive OUT of solid fog
// instead of sitting clearly lit at ~69% opacity. The far PLANE (pm_demo.c's
// far_z) is untouched — it still marks where geometry stops being drawn at
// all, well past where fog has already gone solid, so the optimisation
// half (an invisible cut) still holds.
//
// NEAR_FRAC is not 0 only so the few metres closest to the lens stay clear;
// at exactly 0 even the camera's own position is tinted and the picture
// loses its blacks.
#define FOG_NEAR_FRAC  0.02f
#define FOG_FAR_FRAC   0.52f

// ── The swell ──────────────────────────────────────────────────────────
// Two crossing waves rather than one, because a single wave train reads as
// a corrugated sheet.
//
// Amplitudes are WORLD UNITS, which at 64 to the metre makes these a 55 cm
// and a 34 cm swell — open water on a calm night, and about a thousandth of
// the island's height. The first version of this had them at ~2,200 units
// each, which turned the sea into blue mountains taller than the island and
// filled the whole frame with them. If the water ever looks like terrain,
// this is the number to check first.
// Storm swell: roughly double the calm-night figures.
#define SWELL_A_AMP   72.0f
#define SWELL_B_AMP   46.0f
// Frequencies are per world unit, so these are wavelengths of roughly 1,300
// and 700 units — long, low swell rather than chop.
#define SWELL_A_FREQ  0.00078f
#define SWELL_B_FREQ  0.00142f
#define SWELL_A_SPEED  0.105f
#define SWELL_B_SPEED (-0.072f)

// Foam drift, in texels per second. Slow: the scroll is what supplies the
// sense of direction the texture itself does not have (see tools/
// gen_textures.py's tex_foam), and anything quick reads as a moving
// pattern rather than as water.
#define FOAM_S_SPEED  2.6f
#define FOAM_T_SPEED  1.1f

// ── Sine lookup ────────────────────────────────────────────────────────
// No libm on the hot path — the same stance m64_camera.h takes for its
// damping and pm_fx.h for its shake. 256 entries is a fifth of a degree of
// phase error at this amplitude, which is far below one model unit.
#define SIN_STEPS 256
static float g_sin[SIN_STEPS];
static int   g_sin_ready;

static void sin_table_init(void)
{
    if (g_sin_ready) return;
    // Built once from fm_sinf rather than hand-rolled: fmath's sine inlines
    // and costs nothing here, and this runs once at boot.
    for (int i = 0; i < SIN_STEPS; i++) {
        g_sin[i] = fm_sinf((float)i * (6.28318531f / (float)SIN_STEPS));
    }
    g_sin_ready = 1;
}

static inline float fast_sin(float turns)
{
    // `turns` is in revolutions, not radians — every caller below has a
    // frequency times a coordinate, so revolutions are the natural unit and
    // it saves a divide by 2*pi per lookup.
    int i = (int)(turns * (float)SIN_STEPS);
    return g_sin[i & (SIN_STEPS - 1)];
}

// ── State ──────────────────────────────────────────────────────────────
static M64Transform g_sky_x, g_sea_x, g_bolt_x;
static int          g_xform_ready;

static M64TexAnim   g_foam;
static float        g_swell_t;

// ── Lightning ──────────────────────────────────────────────────────────
// A strike is a short burst of flashes rather than one: real lightning
// flickers as the stepped leader and return strokes fire, and a single
// square pulse reads as a bug in the fade code. Two or three flashes over
// a couple of hundred milliseconds is what sells it.
//
// The schedule is a small xorshift rather than a fixed period. Determinism
// is a BUILD requirement in this project (nix/checks/assets.nix rebuilds and
// compares); at runtime a storm that strikes on a metronome is worse than
// one that does not.
#define STRIKE_MIN_GAP   3.2f
#define STRIKE_MAX_GAP  11.0f
#define FLASH_LEN        0.055f

static uint32_t g_rng = 0x1BADB002u;
static float    g_strike_t;      /* counts down to the next strike        */
static float    g_flash_t;       /* time inside the current strike        */
static int      g_flash_count;   /* flashes left in this strike           */
static float    g_bolt;          /* 0..1 light contribution, this frame   */

static float rnd01(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (float)(g_rng & 0xFFFFFF) / (float)0xFFFFFF;
}

/* Where the current strike came down, and which channel it used. A strike
 * lands anywhere from the middle of the island out onto the water — over
 * land it silhouettes the temple and the palms, over water it puts a hard
 * vertical in an otherwise flat horizon, and both are worth having. */
#define BOLT_VARIANTS  3
#define BOLT_TOP       9000.0f   /* cloud base, world units (~140 m) */
static fm_vec3_t g_bolt_pos;
static int       g_bolt_variant;
static float     g_thunder_t = -1.0f;   /* counts down to the thunder     */
static int       g_thunder_near;

// Craters, for a strike that lands over land (see strike_somewhere's land
// check). Radius is sized against the RESIZED island's own vertex spacing
// (tools/blender/pm_world.py's TERRAIN_DENSITY/SECTORS), not the terrain's
// vertical relief — a wide, shallow bowl reliably catches several
// neighbouring vertices even in the sparser outer bands near the shelf.
// Depth is left at its pre-resize figure on purpose: FIELD_Z/HILL_Z (the
// terrain's own vertical scale) were never rescaled, only the radii were,
// so there is no reason to sink a crater any deeper than before.
#define CRATER_RADIUS     1750.0f  // ~27 m
#define CRATER_DEPTH_MAX   140.0f  // ~2.2 m
static M64CraterField g_terrain_craters;

float pm_env_bolt(void) { return g_bolt; }

static void strike_somewhere(void)
{
    /* Uniform over the disc: sqrt on the radius, or strikes bunch in the
     * middle. Out to twice the land radius, so roughly three quarters of
     * them are over water. */
    const float u = rnd01();
    /* sqrtf, not fm_sqrtf: fmath has no fm_sqrtf because plain sqrtf
     * already compiles to the sqrt.s opcode (fmath.h says so). */
    const float r = PM_LAND_RADIUS * 2.0f * sqrtf(u > 0.0f ? u : 0.0f);
    const float a = rnd01() * 6.2831853f;
    /* Over land the channel stops at the field, over water at the surface. */
    const int over_land = (r < PM_LAND_RADIUS);
    const float y = over_land ? PM_FIELD_Y : 0.0f;
    g_bolt_pos = (fm_vec3_t){ { fm_cosf(a) * r, y, fm_sinf(a) * r } };
    g_bolt_variant = (int)(rnd01() * (float)BOLT_VARIANTS) % BOLT_VARIANTS;

    // A strike over land craters the terrain where it lands (see
    // m64_crater.h) — a no-op if the island's terrain object never
    // resolved (m64_crater_init failed or hasn't run yet).
    if (over_land) {
        m64_crater_impact(&g_terrain_craters, g_bolt_pos.v[0], g_bolt_pos.v[2],
                          CRATER_RADIUS, CRATER_DEPTH_MAX);
    }

    /* Thunder follows the flash by the time sound takes to arrive. At 64
     * units to the metre and 343 m/s that is 21,950 units per second — so
     * a strike on the far side of the water is a beat and a half late.
     * That delay is the whole reason a storm reads as having distance in
     * it, and it costs one float. */
    g_thunder_t = r / 21950.0f;
    g_thunder_near = (r < PM_LAND_RADIUS * 1.1f);
}

// The sea's rest pose. The swell writes absolute positions every frame
// rather than accumulating deltas, so drift and rounding cannot build up
// over a long attract loop.
typedef struct { int16_t x, z; int16_t y0; } PMSeaVert;
static PMSeaVert *g_sea_rest;
static int        g_sea_vert_count;
static T3DVertPacked *g_sea_verts;
static int        g_swell_ok;

static void xforms_init(void)
{
    if (g_xform_ready) return;
    m64_transform_init(&g_sky_x);
    m64_transform_init(&g_sea_x);
    m64_transform_init(&g_bolt_x);
    g_xform_ready = 1;
}

void pm_env_init(void)
{
    sin_table_init();
    xforms_init();

    pm_models_preload(PM_MODEL_SKYDOME);
    pm_models_preload(PM_MODEL_STORM);
    T3DModel *sea = pm_models_get(PM_MODEL_SEA);

    g_foam = (M64TexAnim){
        .mode = M64_TEXANIM_SCROLL,
        .material_name = NULL,  // the sea has exactly one material
        .scroll = { .s_speed = FOAM_S_SPEED, .t_speed = FOAM_T_SPEED },
    };

    // Craters, on the island's own "terrain" object — independent of the
    // sea below, so a missing/failed sea model never skips this. Destroy
    // before re-init: pm_env_init runs again for the beach/arrival shot
    // later in the same session, and m64_crater_init unconditionally
    // overwrites its own state without freeing a prior allocation.
    m64_crater_destroy(&g_terrain_craters);
    T3DModel *island = pm_models_get(PM_MODEL_ISLAND);
    if (island) m64_crater_init(&g_terrain_craters, island, "terrain");

    // Snapshot the rest pose once. Done here rather than lazily in update
    // so that a missing model is a boot-time fact the debug overlay can
    // report, not a per-frame branch.
    if (g_sea_rest) { free(g_sea_rest); g_sea_rest = NULL; }
    g_swell_ok = 0;
    g_sea_verts = NULL;
    g_sea_vert_count = 0;

    if (!sea) return;

    g_sea_verts = t3d_model_get_vertices(sea);
    // Vertices are interleaved two per T3DVertPacked, so the index space is
    // totalVertCount and the struct count is half that — the header at
    // t3dmodel.h:308 spells this out and it is easy to halve twice.
    g_sea_vert_count = sea->totalVertCount;
    if (!g_sea_verts || g_sea_vert_count <= 0) return;

    g_sea_rest = malloc(sizeof(PMSeaVert) * (size_t)g_sea_vert_count);
    if (!g_sea_rest) {
        debugf("pm_env: no memory for the sea's rest pose; swell disabled\n");
        return;
    }
    for (int i = 0; i < g_sea_vert_count; i++) {
        const int16_t *p = t3d_vertbuffer_get_pos(g_sea_verts, i);
        g_sea_rest[i].x  = p[0];
        g_sea_rest[i].y0 = p[1];
        g_sea_rest[i].z  = p[2];
    }
    g_swell_ok = 1;
    debugf("pm_env: swell on %d sea vertices\n", g_sea_vert_count);
}

int pm_env_swell_active(void) { return g_swell_ok; }

void pm_env_night(M64Scene *scene)
{
    // Moonlight. Cool and NOT bright: the moon is a quarter of a stop of
    // sunlight and the whole scene should sit low, with the sky as the
    // brightest thing in frame rather than the terrain.
    // Dropped from 132/148/186 after a capture: the island's beach is
    // authored SAND (196,178,138) for daylight, and at the higher value it
    // came back as a near-white ring round the shore that read as a glow
    // rather than as sand. The asset is right — a value-separated palette
    // is what pm_props.py's _island_colors is for — so the light is what
    // gives way.
    scene->light_color[0] = 96;
    scene->light_color[1] = 112;
    scene->light_color[2] = 150;
    scene->light_color[3] = 0xFF;
    scene->light_dir = (fm_vec3_t){ {
        MOON_EL_COS * MOON_AZ_COS, MOON_EL_SIN, MOON_EL_COS * MOON_AZ_SIN
    } };
    fm_vec3_norm(&scene->light_dir, &scene->light_dir);

    // The fill, from roughly opposite and below — sky light bouncing off
    // the water. Dim, and warmer than the key only so the two sides of a
    // ridge do not read as the same colour at different brightness.
    // Raised after a capture caught the orbit's far side coming back almost
    // black: a third of the flyover looks at the hemisphere the moon is not
    // on, and with only a token fill that reads as a dead frame in an
    // attract reel rather than as night. This is the light that decides
    // whether the shadow side is dark or is missing.
    scene->lights[0].color[0] = 70;
    scene->lights[0].color[1] = 82;
    scene->lights[0].color[2] = 112;
    scene->lights[0].color[3] = 0xFF;
    scene->lights[0].dir = (fm_vec3_t){ {
        -MOON_EL_COS * MOON_AZ_COS, 0.28f, -MOON_EL_COS * MOON_AZ_SIN
    } };
    fm_vec3_norm(&scene->lights[0].dir, &scene->lights[0].dir);

    scene->light_count = 2;

    // Ambient is blue and low. The engine default is grey 45, which over
    // this island's daylight vertex colours reads as an overcast afternoon
    // no matter what the directional lights do.
    /* Overcast ambient: flat, cold and low. Under cloud there is no key
     * light worth the name, so most of what lands on the island comes from
     * here rather than from a direction — which is exactly why a storm
     * reads as shapeless until the lightning gives it one. */
    int amb_r = 24, amb_g = 29, amb_b = 40;

    /* The strike. Lifts ambient hard and briefly whitens it, so for a few
     * frames the island is lit flat and bright from everywhere at once. */
    if (g_bolt > 0.0f) {
        amb_r += (int)(190.0f * g_bolt);
        amb_g += (int)(198.0f * g_bolt);
        amb_b += (int)(214.0f * g_bolt);
        if (amb_r > 255) amb_r = 255;
        if (amb_g > 255) amb_g = 255;
        if (amb_b > 255) amb_b = 255;
    }
    scene->ambient[0] = (uint8_t)amb_r;
    scene->ambient[1] = (uint8_t)amb_g;
    scene->ambient[2] = (uint8_t)amb_b;
    scene->ambient[3] = 0xFF;

    /* The flash lights the fog itself. Leaving the fog colour alone during
     * a strike is the tell that gives away a cheap lightning effect: the
     * world brightens and the haze in front of it does not.
     *
     * Capped well short of white on purpose: the ORIGINAL +150/+155/+165
     * pushed this toward (188,203,239) — a pale colour, at exactly the
     * frame that should feel most oppressive, undoing the storm's own
     * thickness. The AMBIENT boost above is what should light up
     * silhouettes during a strike; the fog colour only needs to stay
     * believable, not turn the whole screen milky. */
    int fr = PM_ENV_HORIZON_R, fg = PM_ENV_HORIZON_G, fb = PM_ENV_HORIZON_B;
    if (g_bolt > 0.0f) {
        fr += (int)(55.0f * g_bolt);
        fg += (int)(60.0f * g_bolt);
        fb += (int)(65.0f * g_bolt);
        if (fr > 170) fr = 170;
        if (fg > 170) fg = 170;
        if (fb > 170) fb = 170;
    }
    const color_t horizon = RGBA32((uint8_t)fr, (uint8_t)fg, (uint8_t)fb, 0xFF);
    // Read from the scene, so the caller's frustum decides the range. The
    // director applies the shot's near/far BEFORE calling this, precisely so
    // this line has something true to read.
    m64_scene_set_fog(scene, horizon,
                      scene->far_z * FOG_NEAR_FRAC,
                      scene->far_z * FOG_FAR_FRAC);

    // The clear colour matters less than it used to now that the dome
    // covers the sky, but it is what shows for the one frame before the
    // dome draws and anywhere the dome does not reach. Same horizon value,
    // so that frame is not a black flash.
    scene->clear_color = horizon;
}

void pm_env_interior(M64Scene *scene)
{
    // The engine's own defaults, restated rather than re-calling
    // m64_scene_init — that would also reset the camera, the FOV and the
    // viewport, which belong to whoever is driving the shot.
    scene->light_color[0] = scene->light_color[1] = scene->light_color[2] = 0xFF;
    scene->light_color[3] = 0xFF;
    scene->light_dir = (fm_vec3_t){ { 1.0f, 1.0f, 1.0f } };
    fm_vec3_norm(&scene->light_dir, &scene->light_dir);
    scene->light_count = 1;

    scene->ambient[0] = scene->ambient[1] = scene->ambient[2] = 45;
    scene->ambient[3] = 0xFF;

    m64_scene_disable_fog(scene);
    scene->clear_color = RGBA32(10, 10, 24, 0xFF);
}

void pm_env_update(float dt)
{
    g_swell_t += dt;
    m64_texanim_update(&g_foam, 1, dt);

    /* ── The storm ──────────────────────────────────────────────────── */
    if (g_thunder_t >= 0.0f) {
        g_thunder_t -= dt;
        if (g_thunder_t < 0.0f) {
            pm_sfx_play(g_thunder_near ? PM_SFX_THUNDER_NEAR
                                       : PM_SFX_THUNDER_FAR);
        }
    }
    g_bolt = 0.0f;
    if (g_flash_count > 0) {
        g_flash_t -= dt;
        /* Alternate on/off through the burst; the last flash is the
         * brightest, which is how a return stroke reads. */
        const float k = (float)g_flash_count;
        g_bolt = (g_flash_count & 1) ? (0.55f + 0.45f / k) : 0.0f;
        if (g_flash_t <= 0.0f) {
            g_flash_count--;
            g_flash_t = FLASH_LEN * (0.6f + rnd01() * 0.9f);
            if (g_flash_count <= 0) {
                g_strike_t = STRIKE_MIN_GAP
                           + rnd01() * (STRIKE_MAX_GAP - STRIKE_MIN_GAP);
            }
        }
    } else {
        g_strike_t -= dt;
        if (g_strike_t <= 0.0f) {
            g_flash_count = 3 + (int)(rnd01() * 3.0f);   /* 3-5 flashes */
            g_flash_t = FLASH_LEN;
            strike_somewhere();
            /* A white pop on the 2D pass as well, so the strike registers
             * even when the camera is looking away from the geometry the
             * light lands on. pm_fx owns the screen; this only asks. */
            pm_fx_flash(RGBA32(196, 206, 226, 255), 0.09f);
        }
    }

    // Independent of the sea's swell below (craters live on the island, not
    // the water) — runs before the swell's early-return so a missing sea
    // model never skips it.
    m64_crater_update(&g_terrain_craters, dt);

    if (!g_swell_ok) return;

    const float pa = g_swell_t * SWELL_A_SPEED;
    const float pb = g_swell_t * SWELL_B_SPEED;

    for (int i = 0; i < g_sea_vert_count; i++) {
        const PMSeaVert *r = &g_sea_rest[i];
        // Model-space coordinates are integers in the tens; the frequencies
        // above are per model unit, so the products land in a sensible
        // number of revolutions.
        // Model units ARE world units here (SEA_SCALE is 1), so the
        // frequencies above apply directly to the stored coordinates.
        const float u = (float)r->x;
        const float v = (float)r->z;

        // ── The swell dies at the shore ────────────────────────────────
        // Without this the water is the same height everywhere, including
        // where it meets the island — and the island's terrain crosses
        // y = 0 in a band around the whole coast. A surface moving +/-57
        // units through sand that is within 57 units of sea level slices
        // in and out of it every cycle, which reads as the entire
        // shoreline shimmering. It is the most visible artefact in the
        // shot and it is not a z-fight; it is real interpenetration.
        //
        // Tapering to zero inside the shore radius is also just what
        // shallow water does. Squared distances throughout: this runs per
        // vertex per frame and the taper does not need a square root to be
        // monotonic.
        const float r2 = u * u + v * v;
        const float lo2 = PM_SHORE_RADIUS * PM_SHORE_RADIUS;
        const float hi2 = (PM_SHORE_RADIUS * 1.6f) * (PM_SHORE_RADIUS * 1.6f);
        float taper = (r2 - lo2) / (hi2 - lo2);
        if (taper <= 0.0f) continue;          /* on the beach: leave it flat */
        if (taper > 1.0f) taper = 1.0f;
        taper = taper * taper * (3.0f - 2.0f * taper);

        const float a = fast_sin(u * SWELL_A_FREQ + v * SWELL_A_FREQ * 0.4f + pa);
        const float b = fast_sin(v * SWELL_B_FREQ - u * SWELL_B_FREQ * 0.3f + pb);

        int16_t *p = t3d_vertbuffer_get_pos(g_sea_verts, i);
        p[1] = (int16_t)(r->y0 +
                         (int)((a * SWELL_A_AMP + b * SWELL_B_AMP) * taper));
    }

    // The RSP DMAs these straight out of RDRAM, so the CPU's dirty lines
    // have to go back first. Without this the sea either does not move (the
    // RSP keeps reading the pre-write values) or tears (some lines evicted,
    // some not) — and both look like a bug somewhere else entirely.
    data_cache_hit_writeback(g_sea_verts,
                             (uint32_t)g_sea_vert_count
                                 * (uint32_t)sizeof(T3DVertPacked) / 2u);
}

void pm_env_draw_sky(const M64Scene *scene)
{
    T3DModel *sky = pm_models_get(PM_MODEL_SKYDOME);
    // A NULL scene means the director has not applied a frustum yet, which
    // happens for exactly one frame at boot. Drawing the dome at the origin
    // instead would put the camera outside it for that frame — a bright
    // flash of the far wall of the sky, which is worse than one frame of
    // clear colour.
    if (!sky || !scene) return;
    xforms_init();

    // Centred on the camera: the dome is a direction field, so it moves
    // with the eye and never gets closer to it.
    g_sky_x.pos = scene->cam_pos;
    g_sky_x.scale = (fm_vec3_t){ { SKY_SCALE, SKY_SCALE, SKY_SCALE } };
    g_sky_x.rot_angle = 0.0f;

    // No DEPTH flag: the sky neither tests nor writes depth, so it can
    // never occlude the world and the world never has to out-sort it. No
    // cull flag either — the dome is seen from the inside, and paying for
    // 408 back-facing triangles is cheaper than a class of bug where the
    // sky is invisible because the winding went the other way.
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_NO_LIGHT);

    // ── And NO FOG on the dome ─────────────────────────────────────────
    // Fog is per-vertex depth, and the dome is camera-centred at a fixed
    // radius — so every one of its vertices is exactly the same distance
    // from the eye. Uniform depth gives uniform fog, which would flatten
    // the whole sky to one colour and take the zenith-to-horizon gradient
    // with it, including the storm's deliberately lighter horizon band.
    //
    // The dome needs no help blending into the fog anyway: its lowest ring
    // IS the fog colour by construction (PM_ENV_HORIZON, shared with
    // pm_env.py). Only the RSP half is toggled — the blender stays armed,
    // so nothing has to be put back but this flag.
    t3d_fog_set_enabled(false);

    m64_transform_push(&g_sky_x);
    t3d_model_draw(sky);
    m64_transform_pop();

    // Hand the pass back exactly as m64_scene_begin set it up.
    t3d_fog_set_enabled(scene->fog_enabled ? true : false);
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH);
}

void pm_env_draw_bolt(const M64Scene *scene)
{
    if (g_bolt <= 0.0f || !scene) return;
    T3DModel *storm = pm_models_get(PM_MODEL_STORM);
    if (!storm) return;

    char name[8] = { 'b','o','l','t','_','0',0,0 };
    name[5] = (char)('0' + g_bolt_variant);
    T3DObject *obj = t3d_model_get_object(storm, name);
    if (!obj) return;

    xforms_init();

    /* Yaw the ribbon to face the camera. The mesh is authored facing +Y in
     * its own space, so this is one angle rather than a full billboard
     * basis — and a bolt is a vertical line, so the only axis that matters
     * is the one it is turned about. */
    const float dx = scene->cam_pos.v[0] - g_bolt_pos.v[0];
    const float dz = scene->cam_pos.v[2] - g_bolt_pos.v[2];
    g_bolt_x.pos = g_bolt_pos;
    g_bolt_x.scale = (fm_vec3_t){ { BOLT_TOP, BOLT_TOP, BOLT_TOP } };
    g_bolt_x.rot_axis = (fm_vec3_t){ { 0.0f, 1.0f, 0.0f } };
    g_bolt_x.rot_angle = fm_atan2f(dx, dz);

    /* Unlit and depth-tested: the channel is its own light source, so
     * shading it would be wrong, but it must still be occluded by the
     * temple when it comes down behind it. */
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT);
    m64_transform_push(&g_bolt_x);
    t3d_model_draw_object(obj, NULL);
    m64_transform_pop();
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH);
}

void pm_env_draw_sea(void)
{
    T3DModel *sea = pm_models_get(PM_MODEL_SEA);
    if (!sea) return;
    xforms_init();

    g_sea_x.pos = (fm_vec3_t){ { 0.0f, 0.0f, 0.0f } };
    g_sea_x.scale = (fm_vec3_t){ { SEA_SCALE, SEA_SCALE, SEA_SCALE } };
    g_sea_x.rot_angle = 0.0f;

    m64_transform_push(&g_sea_x);
    // m64_texanim_draw saves and restores the combiner itself, so the
    // textured water does not leave the RDP in TEX_SHADE for whatever the
    // scene draws next (m64_texanim.h says so explicitly).
    m64_texanim_draw(sea, &g_foam, 1);
    m64_transform_pop();
}
