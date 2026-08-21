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
// not free — and the same reason kiln_skel documents for borrowing its
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

#include "pm_world_gen.h"

// ── The model list ─────────────────────────────────────────────────────
// One list, three consumers: the PMModelId enum, pm_models.c's DFS path
// table, and pm_debug.c's residency-overlay labels.
//
// It is an X-macro because the three used to be three hand-maintained
// lists and they drifted: PM_MODEL_LAB_ARMS landed in the enum with no
// entry in pm_debug.c's names array, which shifted every label from
// "guard" on one slot early and left the array one short of
// PM_MODEL_COUNT. The missing final slot was C-zero-initialised to NULL,
// which reached kiln_gui_text as a format string on the very first frame
// the overlay drew — a boot crash on every debug build, and one that
// `nix build` and `nix flake check` both pass cleanly (7163ca0). Deriving
// all three from one list makes the drift unrepresentable.
//
//   X(id, dfs_path, short_name)
// short_name is the residency overlay's label: four characters, because
// the overlay fits all PM_MODEL_COUNT of them onto one 320 px line.
#define PM_MODEL_LIST(X)                                                     \
    X(PM_MODEL_ISLAND,   "rom:/models/island.t3dm",        "isle")           \
    X(PM_MODEL_PALMS,    "rom:/models/palms.t3dm",         "palm")           \
    X(PM_MODEL_CENTAUR,  "rom:/models/centaur.t3dm",       "cent")           \
    X(PM_MODEL_LOACH,    "rom:/models/loach.t3dm",         "loch")           \
    /* dank_lab.obj, not pm_world.py's procedural box — see pm_lab.h. */     \
    X(PM_MODEL_LAB,      "rom:/models/dank_lab.t3dm",      "lab")            \
    X(PM_MODEL_HORNER,   "rom:/models/horner.t3dm",        "horn")           \
    /* the MRI bay's two idle-animated robotic arms */                       \
    X(PM_MODEL_LAB_ARMS, "rom:/models/lab_arms.t3dm",      "arms")           \
    X(PM_MODEL_GUARD,    "rom:/models/guard_cousin.t3dm",  "guard")          \
    /* The boot splash. Root of DFS for the jingle, models/ for this. */     \
    X(PM_MODEL_KILN_LOGO, "rom:/models/kiln_logo.t3dm",      "logo")           \
    /* The night exterior (pm_env.h): a backdrop dome and the sea. */        \
    X(PM_MODEL_SKYDOME,  "rom:/models/skydome.t3dm",       "sky")            \
    X(PM_MODEL_SEA,      "rom:/models/sea.t3dm",           "sea")            \
    /* lightning channels, bolt_0..2 */                                      \
    X(PM_MODEL_STORM,    "rom:/models/storm.t3dm",         "bolt")

typedef enum {
#define PM_MODEL_ENUM(id, path, name) id,
    PM_MODEL_LIST(PM_MODEL_ENUM)
#undef PM_MODEL_ENUM
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
// beach, when it was 12,813 wide and 4,954 tall. Anything that positions
// the island relative to its own extent should say so in terms of these
// rather than a magic number that reads plausible and is off by 10x.
//
// These used to be literal numbers derived by hand from island_n64.obj's
// authored dimensions. That OBJ has not been the island's source since it
// was replaced by tools/blender/pm_world.py's procedural generator — the
// literals were never updated, so PM_ISLAND_HALF_W (6,406) had already
// drifted from the generator's actual measured PM_ISLAND_RADIUS (8,070 at
// the time this was caught) even before any resize. Deriving them from
// pm_world_gen.h instead means they can never drift again, by the same
// "geometry measures itself" discipline pm_world.py's own header describes.
#define PM_ISLAND_HALF_W  PM_ISLAND_RADIUS  // the generated island's measured radius
#define PM_ISLAND_HEIGHT  PM_ISLAND_TOP     // ...and its measured peak height

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

/** Free one model, leaving the rest of the cache intact. Safe to call on a
 *  model that was never loaded (a no-op) or twice (a no-op the second time).
 *
 *  Exists because "load once, cache forever" (this header's own opening
 *  paragraph) has a cost this project didn't pay attention to until it was
 *  measured: nothing here ever called pm_models_close either, so by late
 *  game the resident set was the UNION of every screen ever visited, not
 *  the current screen's working set — the boot logo and the attract
 *  reel's palms stayed loaded for the rest of the session. Call this for a
 *  model a later screen provably never touches again (the boot splash, the
 *  attract reel's decoration), not for anything shared across screens
 *  (island/lab/centaur/... — freeing those just reintroduces a reload
 *  hitch with no net RAM win, since they get immediately reloaded). */
void pm_models_unload(PMModelId id);

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

/** The short overlay label for a model id. Never NULL — returns "?" out of
 *  range, because the caller hands it to kiln_gui_text as a format string. */
const char *pm_models_name(PMModelId id);

#endif // PM_MODELS_H
