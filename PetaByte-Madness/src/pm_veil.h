// SPDX-License-Identifier: MPL-2.0
//
// pm_veil.h — THE SCARLET VEIL, ported onto libkiln / Tiny3D / rdpq.
//
// A red filter that costs ~zero fill rate, because it is not a filter. It
// is a palette swap. Author both states offline; at runtime change only
// which lookup table you point at, and never touch a pixel you have
// already drawn.
//
//   pixels : swap the TLUT           (32 bytes of DMA, 0 extra pixels shaded)
//   depth  : swap the fog registers  (already in the RSP vertex pipeline)
//   audio  : crossfade two stems     (mixer gain, 0 extra DSP)
//
// The naive version — a full-screen alpha-blended rectangle — costs 76,800
// blended read-modify-writes against the colour buffer every frame at
// 320x240, which is the single most expensive thing you can ask an RDP to
// do, and it buys nothing a TLUT swap does not.
//
// docs/VEIL_DESIGN.md is the design document this implements; read it
// before changing anything here, especially §4 (the palette contract) and
// §6 (why the player ever turns it off).
//
// ── What changed in the port ───────────────────────────────────────────
// The drop's veil.c was written against a generic libdragon with libultra
// gDP calls in comments. Three things are different here:
//
//  1. **Fog goes through Tiny3D, not raw rdpq.** t3d_fog_set_range feeds
//     the RSP vertex pipeline (which is where fog is actually computed on
//     this hardware); rdpq_set_fog_color sets the blender constant. The
//     original's `gSPFogPosition` comment is that pair.
//  2. **The far-plane rebate is applied for real**, by writing KilnScene's
//     far_z — the original computed the lerp and discarded it with a
//     `(void)` cast because it had no scene type to write into. This is
//     where the frame time for the extra demons comes from; do not skip it.
//  3. **pm_veil_ramp_build** is new. The offline CI4 + TLUT pipeline
//     (VEIL_DESIGN.md §8 step 1-2) does not exist in this repo yet, so
//     there are no baked `veil_tluts.h` ramps to point at. Rather than
//     leave the runtime unusable until it does, a ramp can be built at
//     boot by lerping a cold/veiled palette PAIR into the same 9-step
//     layout the baked path uses. Runtime cost is identical — one pointer
//     index per material — and swapping to baked ramps later is a change
//     of one pointer, not of any call site.

#ifndef PM_VEIL_H
#define PM_VEIL_H

#include <stdint.h>
#include <stdbool.h>

#include <kiln/kiln_engine.h>
#include <t3d/t3dmodel.h>
#include <t3d/t3dskeleton.h>

// The veil crossfades over ~0.4 s. Rather than lerp palettes on the VR4300
// every frame, PM_VEIL_STEPS palettes per material are baked at build time
// and indexed. Cost of "animating" the filter: one pointer add.
// 9 steps x 16 entries x 2 bytes = 288 bytes per material.
#define PM_VEIL_STEPS    9
#define PM_VEIL_TLUT_LEN 16  // CI4. Use 256 and CI8 only if you must.

// Material classes. See VEIL_DESIGN.md §4 — under the veil, hue carries no
// information and only value does, so value is rationed: world materials
// collapse to a mid band, demons own true black and true white, and that
// contrast is what physiologically drags the player's eye onto them.
#define PM_VEIL_PAL_WORLD   0x00  // mid value band, hue collapses to red
#define PM_VEIL_PAL_DEMON   0x01  // full value band, owns black and white
#define PM_VEIL_PAL_PHANTOM 0x02  // cold TLUT is alpha 0 on EVERY entry
#define PM_VEIL_PAL_EYES    0x04  // the exception: survives the veil down

typedef struct {
    // [step][entry] RGBA5551. Step 0 is cold, step PM_VEIL_STEPS-1 is full
    // veil. Must be 8-byte aligned for rdpq_tex_upload_tlut.
    const uint16_t (*ramp)[PM_VEIL_TLUT_LEN];
    uint8_t tmem_tile;  // which TLUT tile slot this material owns
    uint8_t flags;      // PM_VEIL_PAL_*
} PMVeilPalette;

// ── The phantom rule ───────────────────────────────────────────────────
// A demon's cold palette is not dim and not low-contrast: every one of its
// 16 entries has alpha 0. In RGBA5551 the alpha bit IS the transparency
// bit, so under an alpha-compare render mode every texel of the creature
// fails the test and no pixel is written.
//
// Which means you should not draw it at all. pm_veil_demon_submit()
// returns false at step 0, the display list is skipped, and the creature
// costs exactly zero while you cannot see it. A corridor can hold a dozen
// of them for free.
//
// The one exception is the eyes: a separate material whose cold palette
// keeps alpha on its top four entries. Four triangles per demon. Veil
// down, you get pinpricks in the dark and nothing else — which is the
// whole reason the trick is frightening rather than merely cheap.

typedef enum {
    PM_VEIL_OFF = 0,
    PM_VEIL_RISING,
    PM_VEIL_ON,
    PM_VEIL_FALLING,
} PMVeilPhase;

typedef struct {
    PMVeilPhase phase;
    float       t;     // 0..1 continuous, for audio + fog
    uint8_t     step;  // 0..PM_VEIL_STEPS-1, quantised, for TLUTs
    float       rate;  // 1/seconds of the crossfade

    // Forced regions: an OVERLORD drags the veil on inside its halo radius.
    // It can pin the veil UP but never down — the elite can expose you, not
    // blind you (VEIL_DESIGN.md §5).
    bool  forced;
    float forced_strength;

    // The draw-distance rebate. Under the veil the horizon is opaque
    // crimson anyway, so the far plane is pulled in while it is up and the
    // triangles are reclaimed for the demons. The filter pays for itself.
    float far_normal;
    float far_veiled;
} PMVeil;

void pm_veil_init  (PMVeil *v, float far_normal, float far_veiled);
void pm_veil_set   (PMVeil *v, bool on);        // player toggle
void pm_veil_force (PMVeil *v, float strength); // OVERLORD halo, 0..1
void pm_veil_update(PMVeil *v, float dt);       // once per frame

/** Once per frame, BEFORE kiln_scene_update: writes the fog colour + range
 *  and the rebated far plane into `scene`. The caller still owns when
 *  matrices rebuild, which is why this does not call kiln_scene_update
 *  itself. */
void pm_veil_apply_scene(const PMVeil *v, KilnScene *scene);

/** Before each material's draw: uploads that material's TLUT for the
 *  current step. Returns false when the material is fully transparent in
 *  this state — skip the display list entirely and save the geometry too. */
bool pm_veil_bind_palette(const PMVeil *v, const PMVeilPalette *p);

/** Which pass should this material go out on right now? */
typedef enum { PM_VEIL_PASS_SKIP, PM_VEIL_PASS_XLU, PM_VEIL_PASS_OPAQUE } PMVeilPass;
PMVeilPass pm_veil_material_pass(const PMVeil *v, const PMVeilPalette *p);

/** Alpha for the XLU pass, 0..255. You cannot crossfade a 1-bit alpha, so
 *  the 400 ms transition runs bodies on an XLU pass with this as prim alpha
 *  and switches to the opaque alpha-compare pass at full veil. */
uint8_t pm_veil_prim_alpha(const PMVeil *v);

/** Screen-edge darkening. NOT a full-screen quad: a ring of 4 trapezoids
 *  (8 triangles) covering only the outer band, ~20,800 blended pixels
 *  instead of 76,800. Optional — cut this first if you are over budget.
 *  Call inside the 2D pass (between kiln_gui_begin and kiln_gui_end). */
void pm_veil_draw_vignette(const PMVeil *v, int screen_w, int screen_h);

/** Should this demon's BODY be submitted at all this frame? The eye
 *  material is submitted unconditionally — it is four triangles. */
bool pm_veil_demon_submit(const PMVeil *v);

/** 0..1 value to hand to the audio side. Under the veil the world loses
 *  its high frequencies and demons keep theirs, so the veil does not just
 *  make demons the only contrast in the room — it makes them the only
 *  sound you can place in space (VEIL_DESIGN.md §7). */
static inline float pm_veil_audio_t(const PMVeil *v) { return v->t; }

/** Build a 9-step ramp from a cold/veiled palette pair, in place, at boot.
 *  `out` is caller storage (288 bytes) and must be 8-byte aligned;
 *  `cold` and `veiled` are 16 RGBA5551 entries each. Interpolates in
 *  5-bit component space, and carries the 1-bit alpha as "opaque once the
 *  step is past halfway" — which is exactly why the crossfade runs on the
 *  XLU pass instead of relying on this alpha.
 *
 *  This is the stand-in for the offline CI4/TLUT bake. When
 *  tools/ grows a real palette generator, point PMVeilPalette::ramp at its
 *  output instead and delete the call — nothing else changes. */
void pm_veil_ramp_build(uint16_t out[PM_VEIL_STEPS][PM_VEIL_TLUT_LEN],
                        const uint16_t cold[PM_VEIL_TLUT_LEN],
                        const uint16_t veiled[PM_VEIL_TLUT_LEN]);

/** Load a baked cold/veiled palette pair from DFS and build its ramp.
 *
 *  `dfs_path` is a `.pal` from nix/assets.nix's mkVeilTexture: 64 bytes, being
 *  16 RGBA5551 big-endian cold entries followed by 16 veiled ones — exactly the
 *  pair pm_veil_ramp_build takes. `ramp` is caller storage (288 bytes) and must
 *  be 8-byte aligned for rdpq_tex_upload_tlut; `out` is filled in on success.
 *
 *  Returns true on success. On a missing or short file it returns false and
 *  leaves `out` untouched — a ROM built without the veil assets should still
 *  boot and play with the filter's fog and far-plane halves working, which is
 *  the state this game shipped in for its whole life so far.
 *
 *  ── Why this exists as a separate call ──────────────────────────────────
 *  This header's own note above says the offline CI4/TLUT bake "does not exist
 *  in this repo yet", and that when tools/ grows a palette generator the change
 *  should be "of one pointer, not of any call site". tools/veil_palette.py and
 *  mkVeilTexture are that generator; this is that one pointer. The runtime
 *  cost is identical either way — one pointer index per material per frame —
 *  and pm_veil_ramp_build is still what expands the nine crossfade steps,
 *  because interpolating 9 x 16 entries once at boot is cheaper than shipping
 *  them and there is nothing about the steps a human needs to art-direct. */
bool pm_veil_load_palette(const char *dfs_path,
                          uint16_t ramp[PM_VEIL_STEPS][PM_VEIL_TLUT_LEN],
                          uint8_t tmem_tile, uint8_t flags,
                          PMVeilPalette *out);

// ── The game's baked palettes ──────────────────────────────────────────
// Built by flake.nix's pmVeilTextures (assetLib.mkVeilTexture over the
// centaur's three already-indexed textures). One registry rather than a load
// per screen, same reasoning pm_models.h gives for models.
typedef enum {
    PM_VEIL_PAL_MC_FACE = 0,
    PM_VEIL_PAL_MC_PLATE,
    PM_VEIL_PAL_MC_GORE,
    PM_VEIL_PAL_COUNT,
} PMVeilPaletteId;

/** Load every baked palette. Call once after DFS is up. Returns how many
 *  loaded; a ROM missing them still runs with the veil's fog and far-plane
 *  halves, which is how this game shipped before the bake existed. */
int pm_veil_palettes_init(void);

/** A loaded palette, or NULL if that one is absent. Hand it straight to
 *  pm_veil_bind_palette. */
const PMVeilPalette *pm_veil_palette(PMVeilPaletteId id);

/** How many loaded, for the debug overlay — "veil-pal 0/3" and "3/3" look
 *  identical on screen and mean completely different things. */
int pm_veil_palettes_loaded(void);

// ── Drawing a model through the veil ───────────────────────────────────
// Binding a TLUT per material means drawing per material, and t3d_model_draw
// draws a whole model in one call with no seam to hook. That is the only reason
// pm_veil_bind_palette / _material_pass / _prim_alpha had no callers: the
// runtime was complete and there was nowhere to put it.
//
// These two provide the seam, using Tiny3D's own per-object iteration
// (t3d_model_iter_* + t3d_model_draw_material + t3d_model_draw_object, which
// its docs name as the way to change material settings mid-model).

/** Which of a model's materials the veil owns. A material not listed here draws
 *  exactly as it would through t3d_model_draw — most of a model normally is not
 *  the veil's business. */
typedef struct {
    const char *material;       /**< material name in the .t3dm */
    const PMVeilPalette *pal;   /**< NULL draws it normally */
} PMVeilMaterial;

/** Draw `model`, binding each listed material's palette for the veil's current
 *  step and honouring pm_veil_material_pass — including SKIP, which does not
 *  walk the display list at all. `bones` may be NULL for an unskinned model.
 *  With `v` or `mats` NULL this is t3d_model_draw. Call inside the 3D pass. */
void pm_veil_draw_model(const PMVeil *v, const T3DModel *model,
                        const T3DMat4FP *bones,
                        const PMVeilMaterial *mats, int count);

/** As above for a skinned model, taking the bone matrices from `skel` the same
 *  way t3d_model_draw_skinned does. Use this instead of kiln_skel_draw for a
 *  character the veil affects. */
void pm_veil_draw_skinned(const PMVeil *v, const T3DModel *model,
                          const T3DSkeleton *skel,
                          const PMVeilMaterial *mats, int count);

/** ── STATE: this path is live, and currently has nothing CI4 to act on ───
 *  The bake, the loader and this draw path all work and are exercised every
 *  frame the centaur is on screen. What is missing is a CI4-TEXTURED MATERIAL:
 *  giving his face/gore/hull/ribs groups `tex0_decal` specs made them render
 *  with no texture sampled at all (a diagnostic build with a pure-green cold
 *  palette produced zero green pixels), and replaced their vertex colours,
 *  so flake.nix leaves those four specs out. See pmCentaurModel's comment for
 *  what was ruled out and what the one-line re-enable is.
 *
 *  Until then pm_veil_draw_centaur binds palettes to materials that do not
 *  sample them — four 32-byte TLUT uploads a frame, no visible effect. That is
 *  deliberate: it is the path that starts working the moment a material is CI4,
 *  and deleting it would mean writing it again.
 */

/** The centaur's veil materials, as a ready-made table for the two calls above.
 *  Fills `out` (which must hold at least 4) and returns how many were filled —
 *  fewer if some palettes are absent, zero if the ROM has none, which is the
 *  case where both draw calls degrade to a plain model draw. */
int pm_veil_centaur_materials(PMVeilMaterial *out, int max);

/** Lend the veil main() owns to the draw sites. Call once at boot. The shots
 *  that draw the centaur are PMDemoShot::draw callbacks taking only `elapsed`,
 *  so they cannot be handed one — same reason pm_demons_bind exists. */
void pm_veil_bind(const PMVeil *v);

/** Draw the centaur through the bound veil, with its palettes already looked
 *  up. Drop-in for kiln_skel_draw at a centaur draw site, and degrades to
 *  exactly that when no veil is bound or no palettes are resident. */
void pm_veil_draw_centaur(const T3DModel *model, const T3DSkeleton *skel);

#endif // PM_VEIL_H
