// SPDX-License-Identifier: MPL-2.0
//
// pm_sfx.h — named sound effects.
//
// ── The contract, for whoever is authoring the sounds ──────────────────
// Every entry in PMSfxId below maps to one file at a fixed path. To make a
// sound real:
//
//   1. Put a mono WAV at PetaByte-Madness/assets/sfx/<name>.wav
//   2. Add one line to flake.nix beside the others:
//        pmSfx "<name>"
//   3. That is all. The id is already wired to its trigger.
//
// The paths below are all under sfx/ because that is where
// assetLib.mkSound puts things. Baked Faust instruments
// (mkBakedInstrument, e.g. the ambience bed and the boot jingle) land at
// the DFS ROOT instead — two builders, two conventions, and mixing them
// up produces silence rather than an error.
//
// Until the file exists, kiln_sfx_load returns -1, pm_sfx_play does
// nothing, and the game runs exactly as it does now. So the triggers can
// be placed and timed against the animation NOW and the sounds dropped in
// later without touching a line of game code — which is the right order,
// because the beat sheet is what a sound has to hit.
//
// ── Priorities and channels ────────────────────────────────────────────
// Channel 0 is the ambience bed (PM_CH_DRONE) and is never allocated from
// here. Everything else is one-shot on an auto-allocated SFX channel, with
// priority-based stealing — so a shotgun landing during a footstep takes
// the footstep's channel rather than being dropped.
//
// The priorities below are deliberate and not all equal: the two kills in
// the beach sequence must never be stolen by anything, because they are
// synchronised to a hit-stop the player can see.

#ifndef PM_SFX_H
#define PM_SFX_H

typedef enum {
    // The beach. Four beats, all timed to the animation in pm_arrival.c.
    PM_SFX_HULL_IMPACT = 0, // the sub hitting sand
    PM_SFX_SHOTGUN,         // `fire`
    PM_SFX_BLADE,           // `slash`
    PM_SFX_SCREAM,          // `shout`

    // The lab.
    PM_SFX_MRI_START,       // the machine taking him
    PM_SFX_PAGE,            // reading a note
    PM_SFX_STEP,            // footsteps; surface-keyed later

    // The storm. Thunder is split near/far because the delay between the
    // flash and the sound is what tells the player how close the strike
    // was — one sample played at two volumes does not sell that, a close
    // crack and a distant roll do.
    PM_SFX_THUNDER_NEAR,
    PM_SFX_THUNDER_FAR,
    PM_SFX_RAIN,            // looping bed, if authored

    // Menus.
    PM_SFX_CURSOR,
    PM_SFX_CONFIRM,

    PM_SFX_COUNT,
} PMSfxId;

/** Load whatever is present. Missing files are not errors — each one
 *  simply stays silent. Call once, after dfs_init and kiln_audio_init. */
void pm_sfx_init(void);

/** Fire a sound. No-op if that sound has no file yet. */
void pm_sfx_play(PMSfxId id);

/** As pm_sfx_play, with an explicit volume (0..1) and pan (0 = left,
 *  0.5 = centre, 1 = right). For anything positional enough to want it but
 *  not positional enough to deserve kiln_sound's full shader path. */
void pm_sfx_play_at(PMSfxId id, float vol, float pan);

/** How many of the sounds actually loaded — for a boot-time debugf, so a
 *  silent build is visible in the log rather than a mystery. */
int pm_sfx_loaded_count(void);

/** Swallow every play while `on`. For pm_cine's seek, which re-runs a shot
 *  from its start at a fixed step to reach an exact time and would otherwise
 *  dump every cue it crosses into the mixer in a single frame. The cue is
 *  still recorded to the trace — only the sound is dropped, because the trace
 *  after a seek should be the shot's full history up to that point. */
void pm_sfx_mute(int on);

#endif // PM_SFX_H
