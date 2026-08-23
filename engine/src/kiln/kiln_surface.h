/* SPDX-License-Identifier: MIT
 *
 * kiln_surface.h — surface properties table. A clean-room analogue of id Tech
 * 4's material/surface-prop system, reduced to what an N64 game can afford.
 *
 * ── One small fixed table, not per-pixel materials ──────────────────────
 * Doom 3 binds textures to named materials and attaches surface flags
 * (metal, flesh, stone) used for bullet decals, ricochet sounds, and
 * friction. On an N64 with no programmable pixel pipeline, the "material"
 * layer is a table of 256 entries indexed by the `hitsurface` field of an
 * KilnTrace. Each entry carries friction, a footstep SFX, and reserved render
 * flags. The actual render primitive (t3d_model_draw, hand-rolled verts)
 * looks up the surface id to decide colour or texture in the demo; a real
 * game uses the table purely for gameplay and keeps Tiny3D combiners for
 * rendering.
 *
 * ── Surface 0 = default ───────────────────────────────────────────────────
 * Unassigned brushes get surface 0. The game should register index 0 as its
 * default (concrete/stone) before loading any maps; otherwise lookups return
 * a zeroed entry (no friction, no footstep). Zeroed is a safe fallback but
 * silent, so the engine prints a debugf on first use of an unregistered id.
 */
#ifndef KILN_SURFACE_H
#define KILN_SURFACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KILN_SURFACE_MAX 256

typedef struct {
    float friction;      /* 0..1-ish, 0.9 = normal, 0.2 = ice, 1.2 = sticky  */
    int   footstep_sfx;  /* kiln_sfx handle, -1 = none                       */
    uint8_t render_flags;
    uint8_t _pad[3];
} KilnSurfaceDef;

/** Register a surface id. Ids 0..255 are available; the game owns the meaning.
 *  Overwriting an already-registered id is allowed (later registration wins)
 *  but produces a debugf warning. */
void kiln_surface_register(uint8_t id, const KilnSurfaceDef *def);

/** Look up a surface. Never returns NULL. An unregistered id returns the
 *  zeroed default and logs a warning on first use. */
const KilnSurfaceDef *kiln_surface_get(uint8_t id);

/** Convenience: play the footstep SFX for a surface at centre-pan, unless it
 *  is -1. Returns the channel used or -1. */
int kiln_surface_play_footstep(uint8_t id, float vol);

#ifdef __cplusplus
}
#endif

#endif /* KILN_SURFACE_H */