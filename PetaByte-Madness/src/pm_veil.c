// SPDX-License-Identifier: MPL-2.0
//
// pm_veil.c — THE SCARLET VEIL, runtime.
//
// Everything expensive happens at build time. This file only ever:
//   - advances one float
//   - picks an index into a pre-baked palette ramp
//   - writes the fog registers and one far-plane float
//   - optionally draws 8 triangles at the screen edge
//
// See pm_veil.h for the design, docs/VEIL_DESIGN.md for why.

#include "pm_veil.h"

#include <libdragon.h>
#include <rdpq_tri.h>
#include <t3d/t3d.h>
#include <t3d/t3dmodel.h>
#include <t3d/t3dskeleton.h>
#include <stdio.h>
#include <string.h>

// The tint. Deep arterial, not fire-engine. Slightly toward magenta so the
// mid band does not collide with the orange of the lab's lamp light.
typedef struct { uint8_t r, g, b; float near_, far_; } PMVeilFog;

static const PMVeilFog FOG_COLD = { 0x0A, 0x0C, 0x10, 900.0f, 1000.0f };
static const PMVeilFog FOG_VEIL = { 0x8E, 0x0C, 0x12, 260.0f,  760.0f };

// ── State ──────────────────────────────────────────────────────────────
void pm_veil_init(PMVeil *v, float far_normal, float far_veiled)
{
    v->phase = PM_VEIL_OFF;
    v->t = 0.0f;
    v->step = 0;
    v->rate = 1.0f / 0.40f;  // 400 ms crossfade, both directions
    v->forced = false;
    v->forced_strength = 0.0f;
    v->far_normal = far_normal;
    v->far_veiled = far_veiled;
}

void pm_veil_set(PMVeil *v, bool on)
{
    if (on && v->phase != PM_VEIL_ON)        v->phase = PM_VEIL_RISING;
    else if (!on && v->phase != PM_VEIL_OFF) v->phase = PM_VEIL_FALLING;
}

void pm_veil_force(PMVeil *v, float strength)
{
    v->forced = strength > 0.0f;
    v->forced_strength = strength;
}

void pm_veil_update(PMVeil *v, float dt)
{
    float target = (v->phase == PM_VEIL_RISING || v->phase == PM_VEIL_ON)
                       ? 1.0f : 0.0f;

    // An OVERLORD in range overrides the player's control. It cannot pull
    // the veil *down*, only up — so the player is never blinded, only
    // exposed, and only ever loses the tool they manage everything else
    // with.
    if (v->forced && v->forced_strength > target) target = v->forced_strength;

    if (v->t < target) {
        v->t += v->rate * dt;
        if (v->t >= target) {
            v->t = target;
            if (target >= 1.0f) v->phase = PM_VEIL_ON;
        }
    } else if (v->t > target) {
        v->t -= v->rate * dt;
        if (v->t <= target) {
            v->t = target;
            if (target <= 0.0f) v->phase = PM_VEIL_OFF;
        }
    }

    // Quantise to a baked step. Deliberately coarse — 9 steps over 400 ms
    // is ~5 frames of visible stepping at 30 fps, which reads as a hard
    // optical snap, like a filter wheel dropping into place, rather than a
    // soft videogame fade. Keep it coarse on purpose.
    float q = v->t * (float)(PM_VEIL_STEPS - 1);
    int s = (int)(q + 0.5f);
    if (s < 0) s = 0;
    if (s > PM_VEIL_STEPS - 1) s = PM_VEIL_STEPS - 1;
    v->step = (uint8_t)s;
}

// ── Scene setup ────────────────────────────────────────────────────────
static inline uint8_t lerp8(uint8_t a, uint8_t b, float t)
{
    return (uint8_t)((float)a + ((float)b - (float)a) * t);
}

static inline float lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

void pm_veil_apply_scene(const PMVeil *v, KilnScene *scene)
{
    const float t = v->t;

    const uint8_t r = lerp8(FOG_COLD.r, FOG_VEIL.r, t);
    const uint8_t g = lerp8(FOG_COLD.g, FOG_VEIL.g, t);
    const uint8_t b = lerp8(FOG_COLD.b, FOG_VEIL.b, t);
    const float near_ = lerpf(FOG_COLD.near_, FOG_VEIL.near_, t);
    const float far_  = lerpf(FOG_COLD.far_,  FOG_VEIL.far_,  t);

    // Fog is the only per-pixel component, and it is free because fog is
    // already in the RSP vertex pipeline (t3d_fog_set_range) and in the
    // blender (rdpq_set_fog_color) for any scene that fogs at all. The
    // libultra equivalent of this pair is gDPSetFogColor + gSPFogPosition,
    // with G_RM_FOG_SHADE_A in cycle 1 of the render mode.
    rdpq_set_fog_color(RGBA32(r, g, b, 0xFF));
    t3d_fog_set_range(near_, far_);
    t3d_fog_set_enabled(true);

    // The rebate. Under the veil the horizon is opaque crimson anyway, so
    // pull the far plane in and reclaim the triangles for the demons.
    // kiln_scene_update rebuilds the projection from this field, so writing
    // it here — before the caller's kiln_scene_update — is the whole wiring.
    scene->far_z = lerpf(v->far_normal, v->far_veiled, t);
}

// ── Palette bind ───────────────────────────────────────────────────────
bool pm_veil_bind_palette(const PMVeil *v, const PMVeilPalette *p)
{
    // Phantom at step 0: every entry is alpha 0, so there is nothing to
    // draw — and no reason to have walked the display list to find out.
    if ((p->flags & PM_VEIL_PAL_PHANTOM) && !(p->flags & PM_VEIL_PAL_EYES)
        && v->step == 0)
        return false;

    const uint16_t *tlut = p->ramp[v->step];

    // 16 entries x 2 bytes = 32 bytes of DMA into TMEM. This is the entire
    // per-material cost of the filter. (libultra: gDPLoadTLUT_pal16.)
    rdpq_tex_upload_tlut((uint16_t *)tlut, p->tmem_tile * PM_VEIL_TLUT_LEN,
                         PM_VEIL_TLUT_LEN);
    rdpq_mode_tlut(TLUT_RGBA16);
    return true;
}

PMVeilPass pm_veil_material_pass(const PMVeil *v, const PMVeilPalette *p)
{
    if (!(p->flags & PM_VEIL_PAL_PHANTOM)) return PM_VEIL_PASS_OPAQUE;
    if (p->flags & PM_VEIL_PAL_EYES)       return PM_VEIL_PASS_XLU; // always lit
    if (v->step == 0)                      return PM_VEIL_PASS_SKIP;
    if (v->step == PM_VEIL_STEPS - 1)      return PM_VEIL_PASS_OPAQUE;
    return PM_VEIL_PASS_XLU;  // mid-crossfade only, ~400 ms
}

uint8_t pm_veil_prim_alpha(const PMVeil *v)
{
    int a = (int)(v->t * 255.0f);
    if (a < 0)   a = 0;
    if (a > 255) a = 255;
    return (uint8_t)a;
}

bool pm_veil_demon_submit(const PMVeil *v)
{
    // Bodies are not drawn at all with the veil down. This is not an
    // optimisation bolted onto the effect — it IS the effect.
    return v->step > 0;
}

// ── Vignette ───────────────────────────────────────────────────────────
// A ring, not a rectangle. Outer edge opaque-ish, inner edge transparent,
// gouraud interpolated between. On a 320x240 buffer with a 20 px band this
// touches ~20,800 pixels instead of 76,800 — about a quarter of the fill
// cost of a full-screen pass, and the only optional component here.
void pm_veil_draw_vignette(const PMVeil *v, int w, int h)
{
    if (v->t <= 0.01f) return;

    const int band = 20;
    const float a = v->t * (150.0f / 255.0f);  // outer alpha

    // 4 trapezoids: top, bottom, left, right. 2 tris each = 8 tris.
    // Corners are covered by the overlap; the double-blend there is free
    // darkening that happens to look correct.
    const struct { float x0, y0, x1, y1, x2, y2, x3, y3; } q[4] = {
        { 0.f, 0.f,        (float)w, 0.f,        (float)(w - band), (float)band,       (float)band,       (float)band       },
        { 0.f, (float)h,   (float)w, (float)h,   (float)(w - band), (float)(h - band), (float)band,       (float)(h - band) },
        { 0.f, 0.f,        (float)band, (float)band, (float)band,   (float)(h - band), 0.f,               (float)h          },
        { (float)w, 0.f,   (float)(w - band), (float)band, (float)(w - band), (float)(h - band), (float)w, (float)h          },
    };

    // Called inside the 2D pass, so depth is already off and the standard
    // combiner is already set — only the blender and combiner need moving,
    // and kiln_gui_end puts them back for the next frame's GUI draws.
    rdpq_set_mode_standard();
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    for (int i = 0; i < 4; i++) {
        // Outer two verts carry the alpha, inner two are transparent.
        float v0[] = { q[i].x0, q[i].y0, 0.f, 0.f, 0.f, a   };
        float v1[] = { q[i].x1, q[i].y1, 0.f, 0.f, 0.f, a   };
        float v2[] = { q[i].x2, q[i].y2, 0.f, 0.f, 0.f, 0.f };
        float v3[] = { q[i].x3, q[i].y3, 0.f, 0.f, 0.f, 0.f };
        rdpq_triangle(&TRIFMT_SHADE, v0, v1, v2);
        rdpq_triangle(&TRIFMT_SHADE, v0, v2, v3);
    }
}

// ── Boot-time ramp build (stand-in for the offline TLUT bake) ───────────
void pm_veil_ramp_build(uint16_t out[PM_VEIL_STEPS][PM_VEIL_TLUT_LEN],
                        const uint16_t cold[PM_VEIL_TLUT_LEN],
                        const uint16_t veiled[PM_VEIL_TLUT_LEN])
{
    for (int s = 0; s < PM_VEIL_STEPS; s++) {
        const float t = (float)s / (float)(PM_VEIL_STEPS - 1);
        for (int e = 0; e < PM_VEIL_TLUT_LEN; e++) {
            const uint16_t c = cold[e], vv = veiled[e];
            // RGBA5551: rrrrrggg ggbbbbba
            const int cr = (c >> 11) & 0x1F, vr = (vv >> 11) & 0x1F;
            const int cg = (c >>  6) & 0x1F, vg = (vv >>  6) & 0x1F;
            const int cb = (c >>  1) & 0x1F, vb = (vv >>  1) & 0x1F;
            const int ca =  c        & 0x01, va =  vv        & 0x01;

            const int r = cr + (int)((float)(vr - cr) * t + 0.5f);
            const int g = cg + (int)((float)(vg - cg) * t + 0.5f);
            const int b = cb + (int)((float)(vb - cb) * t + 0.5f);
            // 1-bit alpha cannot be interpolated: it flips at the halfway
            // step. That is precisely why the crossfade runs on the XLU
            // pass with a prim alpha instead of leaning on this bit.
            const int al = (t < 0.5f) ? ca : va;

            out[s][e] = (uint16_t)((r << 11) | (g << 6) | (b << 1) | al);
        }
    }
}

// ── Loading a baked pair ───────────────────────────────────────────────
bool pm_veil_load_palette(const char *dfs_path,
                          uint16_t ramp[PM_VEIL_STEPS][PM_VEIL_TLUT_LEN],
                          uint8_t tmem_tile, uint8_t flags,
                          PMVeilPalette *out)
{
    if (!dfs_path || !ramp || !out) return false;

    // Probe first. dfs_open asserts through libdragon's must_open on a missing
    // file and kills the ROM — the same trap pm_models.c documents, and the
    // reason the "a missing asset is survivable" contract in the header is
    // actually true rather than aspirational.
    if (!kiln_dfs_exists(dfs_path)) {
        debugf("pm_veil: no %s (fog + far-plane only)\n", dfs_path);
        return false;
    }

    // stdio, NOT dfs_open — and this is the one detail that has to be right.
    // kiln_dfs_exists' own comment says it: dfs_open wants a path WITHOUT the
    // "rom:/" prefix that every caller here writes, while fopen goes through
    // libdragon's filesystem hook and takes the path as spelled. Probing with
    // fopen and then reading with dfs_open meant the probe passed and the open
    // failed on every palette, so the ROM reported "veil-pal 0/3" while the
    // files were demonstrably present. Use one convention for both.
    FILE *f = fopen(dfs_path, "rb");
    if (!f) {
        debugf("pm_veil: fopen failed for %s\n", dfs_path);
        return false;
    }

    // 16 cold + 16 veiled, RGBA5551 BIG-ENDIAN. Read as bytes and assembled
    // explicitly rather than read straight into a uint16_t array: the VR4300 is
    // big-endian so a direct read happens to work today, and writing it out is
    // what keeps it working if this ever runs on a host check.
    uint8_t raw[PM_VEIL_TLUT_LEN * 2 * 2];
    const size_t got = fread(raw, 1, sizeof raw, f);
    fclose(f);

    if (got != sizeof raw) {
        debugf("pm_veil: %s is %u bytes, expected %u\n",
               dfs_path, (unsigned)got, (unsigned)sizeof raw);
        return false;
    }

    uint16_t cold[PM_VEIL_TLUT_LEN], veiled[PM_VEIL_TLUT_LEN];
    for (int i = 0; i < PM_VEIL_TLUT_LEN; i++) {
        cold[i]   = (uint16_t)((raw[i * 2] << 8) | raw[i * 2 + 1]);
        const int j = (PM_VEIL_TLUT_LEN + i) * 2;
        veiled[i] = (uint16_t)((raw[j] << 8) | raw[j + 1]);
    }

    pm_veil_ramp_build(ramp, cold, veiled);

    out->ramp = (const uint16_t (*)[PM_VEIL_TLUT_LEN])ramp;
    out->tmem_tile = tmem_tile;
    out->flags = flags;
    return true;
}

// ── The game's palette registry ────────────────────────────────────────
// One place that owns the loaded pairs, for the same reason pm_models.c owns
// the loaded models: several screens want the centaur, and each loading its own
// copy would put four 288-byte ramps in RAM and make "who owns this" a question
// with four answers.
//
// The ramps must be 8-byte aligned for rdpq_tex_upload_tlut, which is what the
// alignas is for — a misaligned TLUT upload does not fail, it DMAs from the
// wrong offset and the material comes out in a neighbouring palette.
static _Alignas(8) uint16_t g_ramps[PM_VEIL_PAL_COUNT]
                                   [PM_VEIL_STEPS][PM_VEIL_TLUT_LEN];
static PMVeilPalette g_pals[PM_VEIL_PAL_COUNT];
static uint8_t       g_pal_ok[PM_VEIL_PAL_COUNT];

// Order matches PMVeilPaletteId. Every one is `demon` class (see flake.nix's
// pmVeilTextures on why the centaur takes the full value range).
static const struct { const char *path; uint8_t flags; } PAL_SRC[PM_VEIL_PAL_COUNT] = {
    { "rom:/textures/mc_face.pal",  PM_VEIL_PAL_DEMON },
    { "rom:/textures/mc_plate.pal", PM_VEIL_PAL_DEMON },
    { "rom:/textures/mc_gore.pal",  PM_VEIL_PAL_DEMON },
};

int pm_veil_palettes_init(void)
{
    int n = 0;
    for (int i = 0; i < PM_VEIL_PAL_COUNT; i++) {
        // The TMEM tile IS the index: each material owns one 16-entry slot, and
        // two materials sharing a slot would have the second overwrite the
        // first's upload mid-frame.
        // TMEM tile 0 for every palette, not `i`. pm_veil_draw_model draws
        // one material at a time and re-uploads before each, so two palettes
        // never need to be resident together — and a CI4 tile selects its
        // 16-entry block with a palette index f3d_inject does not set, which is
        // 0. Uploading elsewhere would put the entries where the tile is not
        // looking, and the material would render in whatever was at slot 0.
        g_pal_ok[i] = pm_veil_load_palette(PAL_SRC[i].path, g_ramps[i],
                                           0, PAL_SRC[i].flags,
                                           &g_pals[i]) ? 1 : 0;
        n += g_pal_ok[i];
    }
    debugf("pm_veil: %d/%d baked palettes loaded\n", n, PM_VEIL_PAL_COUNT);
    return n;
}

const PMVeilPalette *pm_veil_palette(PMVeilPaletteId id)
{
    if (id < 0 || id >= PM_VEIL_PAL_COUNT || !g_pal_ok[id]) return NULL;
    return &g_pals[id];
}

int pm_veil_palettes_loaded(void)
{
    int n = 0;
    for (int i = 0; i < PM_VEIL_PAL_COUNT; i++) n += g_pal_ok[i];
    return n;
}

// ── Drawing a model THROUGH the veil ───────────────────────────────────
//
// This is the call site the whole palette-swap half of the design was written
// for and never had. `pm_veil_bind_palette`, `_material_pass` and `_prim_alpha`
// existed with zero callers because binding a TLUT per material means drawing
// per material, and `t3d_model_draw` draws the whole model in one go with no
// seam to hook.
//
// Tiny3D already supports the seam: `t3d_model_iter_*` walks the objects and
// `t3d_model_draw_material` applies one object's material before
// `t3d_model_draw_object` draws it — which its own doc names as the way to
// change material settings. So this is that loop with three decisions added per
// object.
//
// ── Order is load-bearing ───────────────────────────────────────────────
// The TLUT upload must come AFTER t3d_model_draw_material, not before: that
// call is what uploads the texture and sets the tile's TLUT mode, and a palette
// bound first is simply overwritten. Nothing warns; the material just renders in
// whatever palette was last resident, which for a CI4 texture is a plausible
// wrong picture rather than an obvious failure.

// ── The callback pair, and why it is not "bind after draw_material" ────
// The first version of this bound the TLUT after t3d_model_draw_material and
// before t3d_model_draw_object. It loaded, ran, rendered the model correctly
// and had NO EFFECT on any pixel: the tile — and with it the TLUT the texels
// are looked up through — is configured inside the draw, so a palette uploaded
// beforehand is simply replaced.
//
// kiln_texanim.h already records the general form of this: "Scrolling and
// palette modes must change tile params per frame, which is impossible with a
// pre-recorded display list", which is why its PALETTE mode uses Tiny3D's
// `tileCb` — the callback documented as hooking "into the tile-setting
// section". That is the only correct place to put a TLUT.
//
// The catch kiln_texanim's own comment admits is that tileCb does not receive
// the material, so it cannot tell which palette to bind. `filterCb` does
// receive the T3DObject — and it is called per object, before that object's
// tiles are set. So the two together are exactly the hook this needs:
//
//   filterCb  picks the object's palette, stashes it, and returns false to
//             DROP the object entirely (which is the phantom rule, for free —
//             a creature you cannot see is never submitted).
//   tileCb    binds whatever filterCb stashed.
//
// The stash is module-static rather than in the context struct because tileCb
// gets the same userData and needs the value filterCb chose for the object
// currently being drawn; Tiny3D drives them strictly in that order per object.
typedef struct {
    const PMVeil *v;
    const PMVeilMaterial *mats;
    int count;
    const PMVeilPalette *pending;   /* chosen by filterCb, read by tileCb */
} VeilDrawCtx;

static const PMVeilPalette *find_mat(const PMVeilMaterial *mats, int count,
                                     const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < count; i++) {
        if (mats[i].material && strcmp(mats[i].material, name) == 0)
            return mats[i].pal;
    }
    return NULL;
}

static bool veil_filter_cb(void *userData, const T3DObject *obj)
{
    VeilDrawCtx *ctx = (VeilDrawCtx *)userData;

    ctx->pending = (obj && obj->material)
                 ? find_mat(ctx->mats, ctx->count, obj->material->name)
                 : NULL;

    // Not a veil material: drawn exactly as it always was. Most of a model is
    // not the veil's business, and an integration that changed how the rest of
    // it looks would be a regression dressed as a feature.
    if (!ctx->pending) return true;

    // PM_VEIL_PASS_SKIP means the display list is never walked at all. This is
    // where the effect pays for itself rather than costing: with the veil down
    // a corridor can hold a dozen creatures for nothing.
    return pm_veil_material_pass(ctx->v, ctx->pending) != PM_VEIL_PASS_SKIP;
}

static void veil_tile_cb(void *userData, rdpq_texparms_t *tileParams,
                         rdpq_tile_t tile)
{
    (void)tileParams; (void)tile;
    VeilDrawCtx *ctx = (VeilDrawCtx *)userData;
    if (ctx->pending) pm_veil_bind_palette(ctx->v, ctx->pending);
}

void pm_veil_draw_model(const PMVeil *v, const T3DModel *model,
                        const T3DMat4FP *bones,
                        const PMVeilMaterial *mats, int count)
{
    if (!model) return;
    if (!v || !mats || count <= 0) {
        // No veil state or no veil materials: this is just a model.
        t3d_model_draw_custom(model, (T3DModelDrawConf){ .matrices = bones });
        return;
    }

    VeilDrawCtx ctx = { .v = v, .mats = mats, .count = count, .pending = NULL };
    t3d_model_draw_custom(model, (T3DModelDrawConf){
        .userData = &ctx,
        .filterCb = veil_filter_cb,
        .tileCb   = veil_tile_cb,
        .matrices = bones,
    });

    // ── On the 400 ms crossfade ─────────────────────────────────────────
    // pm_veil_prim_alpha is NOT used here. Fading a body in needs the body's
    // alpha to come from PRIM, and the material's authored combiner
    // (tex0_decal) sources alpha from the texture — deliberately, because
    // VEIL_DESIGN section 9 requires demons to draw near-DECAL so they keep the
    // bright end of the palette. Overriding the combiner from a tile callback
    // would fight the material Tiny3D is in the middle of applying.
    //
    // What happens instead is pm_veil_ramp_build's documented fallback: the
    // ramp's 1-bit alpha flips at the halfway step, so a phantom appears on the
    // step that crosses 0.5 rather than fading in over nine. On a 400 ms
    // transition that is one frame's difference in feel and it is honest about
    // what the hardware does with a 1-bit alpha. A real fade wants a second
    // material variant with a PRIM alpha term, which is a change to the
    // ASSET's combiner rather than to this file.
}

void pm_veil_draw_skinned(const PMVeil *v, const T3DModel *model,
                          const T3DSkeleton *skel,
                          const PMVeilMaterial *mats, int count)
{
    if (!skel) { pm_veil_draw_model(v, model, NULL, mats, count); return; }

    // Exactly what t3d_model_draw_skinned does to pick its matrices, restated
    // because that helper takes no draw config and so cannot be used here.
    // A buffered skeleton addresses its matrices through a segment, which
    // t3d_skeleton_use is what installs.
    t3d_skeleton_use(skel);
    const T3DMat4FP *bones =
        skel->bufferCount == 1
            ? skel->boneMatricesFP
            : (const T3DMat4FP *)t3d_segment_placeholder(T3D_SEGMENT_SKELETON);

    pm_veil_draw_model(v, model, bones, mats, count);
}

// ── The centaur's veil materials ───────────────────────────────────────
// Which .t3dm material each baked palette belongs to. The names are the
// machine_centaur.json prim groups that centaur.py turns into Blender
// materials, and the same names flake.nix's pmCentaurModel gives `tex0_decal`
// specs for — three places that must agree, and this is the one the runtime
// reads. A typo here is silent: the material simply never gets a palette and
// draws unveiled, which looks like the veil not working rather than like a
// name being wrong.
//
// `hull` and `ribs` share mc_plate. That is fine and deliberate: the palette is
// bound per DRAW, not per material, so two materials pointing at one ramp
// upload it twice and cost 32 bytes of DMA extra.
static const struct { const char *material; PMVeilPaletteId id; }
CENTAUR_MATS[] = {
    { "face", PM_VEIL_PAL_MC_FACE  },
    { "gore", PM_VEIL_PAL_MC_GORE  },
    { "hull", PM_VEIL_PAL_MC_PLATE },
    { "ribs", PM_VEIL_PAL_MC_PLATE },
};

int pm_veil_centaur_materials(PMVeilMaterial *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < (int)(sizeof CENTAUR_MATS / sizeof CENTAUR_MATS[0]); i++) {
        if (n >= max) break;
        const PMVeilPalette *p = pm_veil_palette(CENTAUR_MATS[i].id);
        // An absent palette is skipped rather than listed with a NULL: a
        // listed-but-NULL entry would make pm_veil_draw_model take its veil
        // branch and then bind nothing. Skipping leaves the material on the
        // ordinary draw path, which is exactly right for a ROM built without
        // the veil assets.
        if (!p) continue;
        out[n].material = CENTAUR_MATS[i].material;
        out[n].pal = p;
        n++;
    }
    return n;
}

// ── The veil main() owns, borrowed by the draw sites ───────────────────
// Same arrangement pm_demons_bind uses, and for the same reason: the PMVeil
// lives on main()'s stack because it is per-session state, and the shots that
// draw the centaur (pm_demo.c's reel, pm_arrival.c's beach) are called through
// PMDemoShot::draw, whose signature is `void (*)(float elapsed)`. Threading a
// veil pointer through every shot callback so that two of them can use it is
// the wrong trade.
static const PMVeil *g_current;

void pm_veil_bind(const PMVeil *v) { g_current = v; }

void pm_veil_draw_centaur(const T3DModel *model, const T3DSkeleton *skel)
{
    PMVeilMaterial mats[4];
    const int n = pm_veil_centaur_materials(mats, 4);

    // No veil bound or no palettes resident: identical to the kiln_skel_draw
    // this replaced. That is the state a ROM built without the veil assets is
    // in, and it must still draw the character.
    if (!g_current || n == 0) {
        t3d_skeleton_use(skel);
        t3d_model_draw_skinned(model, skel);
        return;
    }
    pm_veil_draw_skinned(g_current, model, skel, mats, n);
}
