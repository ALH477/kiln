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

void pm_veil_apply_scene(const PMVeil *v, M64Scene *scene)
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
    // m64_scene_update rebuilds the projection from this field, so writing
    // it here — before the caller's m64_scene_update — is the whole wiring.
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
    // and m64_gui_end puts them back for the next frame's GUI draws.
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
