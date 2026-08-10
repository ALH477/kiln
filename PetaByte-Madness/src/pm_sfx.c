// SPDX-License-Identifier: MPL-2.0
//
// pm_sfx.c — see pm_sfx.h.

#include "pm_sfx.h"

#include <libdragon.h>
#include <m64/m64_audio.h>
#include <m64/m64_engine.h>

#include "pm_screens.h"  // PM_CH_DRONE — the one channel never used here

typedef struct {
    const char *path;
    uint8_t     priority;
} PMSfxDef;

// Indexed by PMSfxId. Paths are the contract documented in pm_sfx.h.
static const PMSfxDef DEFS[PM_SFX_COUNT] = {
    // The two kills outrank everything: they are synchronised to a
    // hit-stop the player can see, and a stolen channel there reads as the
    // game having skipped rather than as a missing sound.
    [PM_SFX_HULL_IMPACT] = { "rom:/sfx/hull_impact.wav64", 230 },
    [PM_SFX_SHOTGUN]     = { "rom:/sfx/shotgun.wav64",     250 },
    [PM_SFX_BLADE]       = { "rom:/sfx/blade.wav64",       250 },
    [PM_SFX_SCREAM]      = { "rom:/sfx/scream.wav64",      240 },

    [PM_SFX_MRI_START]   = { "rom:/sfx/mri_start.wav64",   200 },
    [PM_SFX_PAGE]        = { "rom:/sfx/page.wav64",         90 },
    [PM_SFX_STEP]        = { "rom:/sfx/step.wav64",         40 },

    [PM_SFX_CURSOR]      = { "rom:/sfx/cursor.wav64",      120 },
    [PM_SFX_CONFIRM]     = { "rom:/sfx/confirm.wav64",     130 },
};

static int g_handle[PM_SFX_COUNT];
static int g_loaded;

void pm_sfx_init(void)
{
    g_loaded = 0;
    for (int i = 0; i < PM_SFX_COUNT; i++) {
        // Probe before loading. wav64_open asserts through libdragon's
        // must_open on a missing file, so an unauthored sound would kill the
        // ROM at boot rather than run silent — and none of these nine exist
        // yet. The check lives here, in the module that knows these assets
        // are optional, rather than in the engine.
        g_handle[i] = (DEFS[i].path && m64_dfs_exists(DEFS[i].path))
                        ? m64_sfx_load(DEFS[i].path) : -1;
        if (g_handle[i] >= 0) g_loaded++;
    }
    // Loud in the log, silent in the game. A build with no sounds is a
    // legitimate state right now (they are being authored separately), so
    // this is a note rather than a warning — but it should never be a
    // surprise that a ROM shipped mute.
    debugf("pm_sfx: %d/%d sounds loaded\n", g_loaded, PM_SFX_COUNT);
}

int pm_sfx_loaded_count(void) { return g_loaded; }

void pm_sfx_play(PMSfxId id)
{
    if (id < 0 || id >= PM_SFX_COUNT || g_handle[id] < 0) return;
    // -1 channel: let the mixer allocate and steal by priority. The
    // ambience bed on PM_CH_DRONE is outside the pool this draws from
    // because it was started explicitly on its own channel.
    m64_sfx_play(g_handle[id], -1, DEFS[id].priority);
}

void pm_sfx_play_at(PMSfxId id, float vol, float pan)
{
    if (id < 0 || id >= PM_SFX_COUNT || g_handle[id] < 0) return;
    const int ch = m64_sfx_play(g_handle[id], -1, DEFS[id].priority);
    if (ch >= 0) m64_sfx_set_vol_pan(ch, vol, pan);
}
