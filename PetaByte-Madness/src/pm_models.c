// SPDX-License-Identifier: MPL-2.0
//
// pm_models.c — see pm_models.h.

#include "pm_models.h"

#include <libdragon.h>
#include <kiln/kiln_engine.h>

static T3DModel *g_models[PM_MODEL_COUNT];
static uint8_t g_tried[PM_MODEL_COUNT];

// Both tables are generated from PM_MODEL_LIST (pm_models.h), so a new
// model cannot be added to the enum without also getting a path and an
// overlay label. Built by tools/blender/pm_props.py and
// tools/blender/centaur.py; see PetaByte-Madness/docs/ASSET_PIPELINE.md.
#define PM_MODEL_PATH(id, path, name) [id] = path,
static const char *const PATHS[PM_MODEL_COUNT] = {
    PM_MODEL_LIST(PM_MODEL_PATH)
};
#undef PM_MODEL_PATH

#define PM_MODEL_NAME(id, path, name) [id] = name,
static const char *const NAMES[PM_MODEL_COUNT] = {
    PM_MODEL_LIST(PM_MODEL_NAME)
};
#undef PM_MODEL_NAME

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
    if (!kiln_dfs_exists(PATHS[id])) {
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

const char *pm_models_name(PMModelId id)
{
    // "?" rather than NULL: the only caller is a debug overlay that hands
    // this straight to kiln_gui_text as a format string, and returning NULL
    // there is precisely the boot crash PM_MODEL_LIST exists to prevent.
    // A guard that reintroduces the failure it was written to stop is worse
    // than no guard at all.
    if (id < 0 || id >= PM_MODEL_COUNT) return "?";
    return NAMES[id];
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

void pm_models_unload(PMModelId id)
{
    if (id < 0 || id >= PM_MODEL_COUNT) return;
    if (g_models[id]) {
        t3d_model_free(g_models[id]);
        g_models[id] = NULL;
    }
    // Reset g_tried too, not just g_models: leaving it set would make a
    // later pm_models_get for this id return NULL forever (the "only try
    // once" guard above), which is correct for a genuinely missing asset
    // but wrong here — this model was never missing, it was deliberately
    // freed, and a screen that (incorrectly) asks for it again should get
    // a fresh load, not a permanent silent NULL.
    g_tried[id] = 0;
}
