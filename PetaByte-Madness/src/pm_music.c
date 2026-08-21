// SPDX-License-Identifier: MPL-2.0
//
// pm_music.c — see pm_music.h.

#include "pm_music.h"

#include <libdragon.h>
#include <kiln/kiln_audio.h>
#include <kiln/kiln_engine.h>

#include "pm_screens.h"  // PM_CH_MUSIC

#define XM_PATH     "rom:/music/petabyte.xm64"
#define STREAM_PATH "rom:/music/petabyte_stream.wav64"

// The score runs 64.0 s exactly: 512 rows at speed 6 / 120 BPM, which is
// 128 beats at 120 bpm. tools/midi_to_xm.py prints this when it converts,
// and it is a property of the module rather than of this playback, so the
// clock is the PRIMARY end-of-song test here.
//
// It is primary because the obvious test is not trustworthy: a tracker
// channel only counts as "playing" while a note sounds on it, so a quartet
// with every voice resting on the same beat reads as a finished song. That
// is why the silence test below has to persist for QUIET_HANDOVER seconds
// before it counts — it is the backstop for a module that ends early, not
// the mechanism.
#define XM_SECONDS      64.0f
#define QUIET_HANDOVER   2.5f

enum { ST_SILENT = 0, ST_SCORE, ST_STREAM };

static int   g_xm     = -1;
static int   g_stream = -1;
static int   g_state  = ST_SILENT;
static int   g_active;
static float g_elapsed;
static float g_quiet;   // how long every voice has been silent

void pm_music_init(void)
{
    // Same probe-first rule as pm_sfx: both of these are optional.
    g_xm     = kiln_dfs_exists(XM_PATH)     ? kiln_music_load(XM_PATH)   : -1;
    g_stream = kiln_dfs_exists(STREAM_PATH) ? kiln_sfx_load(STREAM_PATH) : -1;
    g_state  = ST_SILENT;
    g_active = 0;

    if (g_xm >= 0) {
        // One pass, not a loop: the whole point is that it ENDS and hands
        // over to the recording. kiln_music_play loops by default.
        kiln_music_set_loop(g_xm, 0);
    }
    debugf("pm_music: score %s, recording %s\n",
           g_xm >= 0 ? "ok" : "MISSING",
           g_stream >= 0 ? "ok" : "MISSING");
}

static void start_score(void)
{
    g_elapsed = 0.0f;
    g_quiet   = 0.0f;
    if (g_xm >= 0) {
        kiln_music_set_loop(g_xm, 0);
        kiln_music_play(g_xm);
        g_state = ST_SCORE;
        return;
    }
    // No score in this build: go straight to the recording rather than
    // leaving the title silent.
    g_state = ST_SILENT;
}

static void start_stream(void)
{
    g_elapsed = 0.0f;
    g_quiet   = 0.0f;
    if (g_stream >= 0) {
        // Priority 255 on a fixed channel, exactly like the ambience bed:
        // kiln_sfx_play_ex only steals a channel whose priority is strictly
        // lower, so nothing in pm_sfx can take this one mid-song.
        kiln_sfx_play(g_stream, PM_CH_MUSIC, 255);
        g_state = ST_STREAM;
        return;
    }
    g_state = ST_SILENT;
}

void pm_music_stop(void)
{
    if (g_xm >= 0) kiln_music_stop(g_xm);
    if (g_stream >= 0) kiln_sfx_stop(PM_CH_MUSIC);
    g_state = ST_SILENT;
    g_elapsed = 0.0f;
    g_quiet   = 0.0f;
}

void pm_music_set_active(int active)
{
    active = active ? 1 : 0;
    if (active == g_active) return;
    g_active = active;
    if (active) start_score();
    else        pm_music_stop();
}

void pm_music_update(float dt, float vol)
{
    if (!g_active) return;
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    g_elapsed += dt;

    switch (g_state) {
    case ST_SCORE:
        kiln_music_set_volume(g_xm, vol);
        // The CLOCK is the whole test. kiln_music_playing reports whether
        // the track's FIRST mixer channel has a note sounding, so on a
        // quartet whose lead rests for bars at a time it reads "stopped"
        // most of the piece — it fired the handover 11 seconds into a
        // 64-second score. The score's length is a property of the module
        // (tools/midi_to_xm.py prints it), so it is known here exactly and
        // does not need to be inferred from the mixer at all.
        (void)g_quiet;
        if (g_elapsed >= XM_SECONDS) {
            kiln_music_stop(g_xm);
            start_stream();
        }
        break;

    case ST_STREAM:
        kiln_sfx_set_vol_pan(PM_CH_MUSIC, vol, 0.5f);
        // The wav64 is built without a baked loop point, so the loop is
        // here: when the channel goes quiet, play it again. Same one-frame
        // startup race as above, hence the elapsed guard.
        if (g_elapsed > 1.0f && !kiln_sfx_playing(PM_CH_MUSIC)) {
            start_stream();
        }
        break;

    case ST_SILENT:
    default:
        break;
    }
}

int pm_music_state(void)
{
    if (g_xm < 0 && g_stream < 0) return -1;
    return g_state;
}
