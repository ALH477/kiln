// SPDX-License-Identifier: MPL-2.0
//
// pm_models.c — see pm_models.h.

#include "pm_models.h"

#include <libdragon.h>
#include <m64/m64_engine.h>

static T3DModel *g_models[PM_MODEL_COUNT];
static uint8_t g_tried[PM_MODEL_COUNT];

// Indexed by PMModelId. Built by tools/blender/pm_props.py and
// tools/blender/centaur.py; see PetaByte-Madness/docs/ASSET_PIPELINE.md.
static const char *const PATHS[PM_MODEL_COUNT] = {
    [PM_MODEL_ISLAND]  = "rom:/models/island.t3dm",
    [PM_MODEL_PALMS]   = "rom:/models/palms.t3dm",
    [PM_MODEL_CENTAUR] = "rom:/models/centaur.t3dm",
    [PM_MODEL_LOACH]   = "rom:/models/loach.t3dm",
    [PM_MODEL_LAB]     = "rom:/models/lab.t3dm",
    [PM_MODEL_HORNER]  = "rom:/models/horner.t3dm",
    [PM_MODEL_GUARD]   = "rom:/models/guard_cousin.t3dm",
    // The boot splash. Root of DFS for the jingle, models/ for this.
    [PM_MODEL_M64_LOGO] = "rom:/models/m64_logo.t3dm",
    [PM_MODEL_SKYDOME] = "rom:/models/skydome.t3dm",
    [PM_MODEL_SEA]     = "rom:/models/sea.t3dm",
};

T3DModel *pm_models_get(PMModelId id)
{
    if (id < 0 || id >= PM_MODEL_COUNT) return NULL;
    if (g_models[id]) return g_models[id];

    // Only try once: a missing asset should cost one lookup per session,
    // not one per frame.
    if (g_tried[id]) return NULL;
    g_tried[id] = 1;

    // t3d_model_load does NOT return NULL for a missing file — it asserts
    // through libdragon's must_open and kills the ROM. Probing first is
    // what actually makes the "NULL is survivable" contract in the header
    // true; without it the first absent model is a boot crash.
    if (!m64_dfs_exists(PATHS[id])) {
        debugf("pm_models: missing %s\n", PATHS[id]);
        return NULL;
    }

    g_models[id] = t3d_model_load(PATHS[id]);
    if (!g_models[id]) debugf("pm_models: missing %s\n", PATHS[id]);
    return g_models[id];
}

void pm_models_preload(PMModelId id) { (void)pm_models_get(id); }

int pm_models_status(PMModelId id)
{
    if (id < 0 || id >= PM_MODEL_COUNT) return 0;
    if (g_models[id]) return +1;
    return g_tried[id] ? -1 : 0;
}

const char *pm_models_path(PMModelId id)
{
    if (id < 0 || id >= PM_MODEL_COUNT) return NULL;
    return PATHS[id];
}

void pm_models_close(void)
{
    for (int i = 0; i < PM_MODEL_COUNT; i++) {
        if (g_models[i]) {
            t3d_model_free(g_models[i]);
            g_models[i] = NULL;
        }
        g_tried[i] = 0;
    }
}
