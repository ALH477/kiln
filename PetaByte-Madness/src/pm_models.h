// SPDX-License-Identifier: MPL-2.0
//
// pm_models.h — one place that owns every T3DModel the game loads.
//
// Four modules want the centaur (the attract reel, the intake, the beach,
// and gameplay), three want the island, two want the LOACH. Each loading
// its own copy would put several hundred KB of duplicate geometry in a
// 4 MB console's RAM and make "who frees this" a question with four
// answers.
//
// So: load once, borrow by id, free at the end. Same contract T3DModel
// itself has — pm_models_get returns a borrowed pointer the caller must
// not free — and the same reason m64_skel documents for borrowing its
// `model` rather than owning it.
//
// ── Loaded on demand, not all at boot ──────────────────────────────────
// The title screen needs the island and the palms; it does not need the
// lab, Horner, or the guards. Loading everything at boot would cost RAM
// for content the player may never reach in this session (a returning
// player with a save never sees the lab at all). pm_models_get loads on
// first use and caches, so a screen pays only for what it draws.
//
// The cost of that choice is a visible hitch the first time a model is
// asked for. Every caller here asks during a fade or a black frame, which
// is where the hitch is free — see pm_screens.c's transitions. If a model
// is ever needed mid-gameplay, preload it at the transition into that
// scene rather than making the fetch lazy at the point of use.

#ifndef PM_MODELS_H
#define PM_MODELS_H

#include <t3d/t3dmodel.h>

typedef enum {
    PM_MODEL_ISLAND = 0,
    PM_MODEL_PALMS,
    PM_MODEL_CENTAUR,
    PM_MODEL_LOACH,
    PM_MODEL_LAB,
    PM_MODEL_HORNER,
    PM_MODEL_GUARD,
    PM_MODEL_M64_LOGO,
    // The night exterior (pm_env.h): a backdrop dome and the sea.
    PM_MODEL_SKYDOME,
    PM_MODEL_SEA,
    PM_MODEL_STORM,   // lightning channels, bolt_0..2
    PM_MODEL_COUNT,
} PMModelId;

// ── Model dimensions, in world units ───────────────────────────────────
// This world runs at 64 units to the metre: pm_lab.h states the convention
// and gltf_to_t3d's --base-scale=64 (nix/blender.nix) enforces it on every
// model in the game. A model's world size is therefore fixed by its source
// and its tools/blender/pm_props.py scale, and is NOT something a call site
// gets to guess at.
//
// These constants exist because guessing at them is exactly what went
// wrong: the island was placed as though it were a prop a few hundred units
// across, first at 3,200 units in the sub shot and then at 900 in the
// beach, when it is 12,813 wide and 4,954 tall. Anything that positions the
// island relative to its own extent should say so in terms of these rather
// than a magic number that reads plausible and is off by 10x.
//
//   island_n64.obj  26 x 10.05 x 26 units authored
//                 x 7.7  (pm_props.py "island" scale) = 200 x 77 x 200 m
//                 x 64   (--base-scale)               = the numbers below
#define PM_ISLAND_HALF_W  6406.0f   // 13 * 7.7 * 64
#define PM_ISLAND_HEIGHT  4954.0f   // 10.05 * 7.7 * 64

// A palm out of n64_florida_keys_palms.gltf is authored in metres and is
// 6.06 m at its tallest, so it lands at 388 units unscaled.
#define PM_PALM_HEIGHT     388.0f

/** Borrowed pointer, loaded on first use. NULL if the asset is missing —
 *  every caller must handle that, because a ROM built without one of these
 *  should still boot and show the rest rather than hang on a black screen. */
T3DModel *pm_models_get(PMModelId id);

/** Load now rather than at first draw. Call during a fade or a black
 *  frame; see the header comment on where the hitch is free. */
void pm_models_preload(PMModelId id);

/** Free everything. Safe to call twice. */
void pm_models_close(void);

/** What happened to a model, without asking for it: 0 not requested yet,
 *  +1 loaded, -1 asked for and absent. The debug overlay reads this — a
 *  model that silently resolved to NULL draws nothing, which on a dark
 *  screen is indistinguishable from a camera pointed at the wrong place,
 *  and telling those two apart by reading source has already cost this
 *  project days. Never loads: querying must not have the side effect the
 *  thing being diagnosed might be about. */
int pm_models_status(PMModelId id);

/** The DFS path for a model id, for the same overlay. NULL if out of range. */
const char *pm_models_path(PMModelId id);

#endif // PM_MODELS_H
