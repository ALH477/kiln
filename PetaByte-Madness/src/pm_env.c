// SPDX-License-Identifier: MPL-2.0
//
// pm_env.c — see pm_env.h.

#include "pm_env.h"

#include <libdragon.h>
#include <stdlib.h>
#include <t3d/t3d.h>
#include <t3d/t3dmodel.h>

#include <m64/m64_texanim.h>

#include "pm_models.h"
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
// These are distances from the CAMERA, and the flyover's camera orbits
// 16,000-19,000 units out from an island 12,813 across. So the island
// itself spans roughly 10,000 to 26,000 from the lens, and the sea's far
// rim reaches about 38,000.
//
// The first attempt used 5,000-20,000 — reasoning about distances from the
// island's centre rather than from the eye. That put the whole island past
// the far plane of the fog, so it drew as a flat silhouette in exactly the
// horizon colour and read as a hole in the sea with sky showing through.
// Fog ranges are camera-relative; a range that sounds right for the size of
// the subject is usually wrong for the distance to it.
//
// Now: nothing fogs until past the island's near shore, the far side of the
// island carries enough haze to read as depth, and the sea's outer rim is
// fully gone before it ends.
#define FOG_NEAR  15000.0f
#define FOG_FAR   40000.0f

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
#define SWELL_A_AMP   35.0f
#define SWELL_B_AMP   22.0f
// Frequencies are per world unit, so these are wavelengths of roughly 1,300
// and 700 units — long, low swell rather than chop.
#define SWELL_A_FREQ  0.00078f
#define SWELL_B_FREQ  0.00142f
#define SWELL_A_SPEED  0.055f
#define SWELL_B_SPEED (-0.037f)

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
static M64Transform g_sky_x, g_sea_x;
static int          g_xform_ready;

static M64TexAnim   g_foam;
static float        g_swell_t;

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
    g_xform_ready = 1;
}

void pm_env_init(void)
{
    sin_table_init();
    xforms_init();

    pm_models_preload(PM_MODEL_SKYDOME);
    T3DModel *sea = pm_models_get(PM_MODEL_SEA);

    g_foam = (M64TexAnim){
        .mode = M64_TEXANIM_SCROLL,
        .material_name = NULL,  // the sea has exactly one material
        .scroll = { .s_speed = FOAM_S_SPEED, .t_speed = FOAM_T_SPEED },
    };

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
    scene->ambient[0] = 26;
    scene->ambient[1] = 32;
    scene->ambient[2] = 50;
    scene->ambient[3] = 0xFF;

    const color_t horizon = RGBA32(PM_ENV_HORIZON_R, PM_ENV_HORIZON_G,
                                   PM_ENV_HORIZON_B, 0xFF);
    m64_scene_set_fog(scene, horizon, FOG_NEAR, FOG_FAR);

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

    m64_transform_push(&g_sky_x);
    t3d_model_draw(sky);
    m64_transform_pop();

    // Hand the pass back exactly as m64_scene_begin set it up.
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
