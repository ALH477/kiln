// SPDX-License-Identifier: MPL-2.0
//
// pm_screens.h — the whole-game screen state machine.
//
// Modelled on game/src/gg_screens.h and for the same reason: one app
// struct on main()'s stack, screens as an enum rather than function
// pointers into modules with their own statics. Every screen here reads
// and writes the same few things (which profile is selected, how far the
// intro has got, what the camera is doing), and threading that through
// callbacks would be the same struct with more indirection.
//
// ── The shape ──────────────────────────────────────────────────────────
//
//   BOOT ──► TITLE ◄──────────── ATTRACT        (idle ~12 s / any button)
//              │
//              ▼
//            FILE  ── filled slot ──────────────────────► PLAY
//              │
//              └── empty slot ──► the intro ──► PLAY
//
//                  LAB_CINE ─► LAB ─► INTAKE ─► SUB ─► BEACH
//
// The intro plays on a NEW PROFILE ONLY, which is what makes the file
// select the gate rather than a formality — the same thing Ocarina of
// Time does, and the reason its opening only exists for a player who has
// not seen it.
//
// ── Where the intro's beats live ───────────────────────────────────────
// This module owns WHICH screen is up and what drives the camera on it.
// It does not own the beats:
//   * LAB_CINE / SUB / BEACH are PMDemoShots (pm_demo.h) — keyframed
//     cameras over live geometry.
//   * LAB is the playable section; pm_lab.c owns it.
//   * INTAKE is the transformation; pm_intake.c owns it.
// A screen here is a few lines of "start that, wait for it, go to the
// next one".
//
// ── START skips ────────────────────────────────────────────────────────
// Every intro screen honours START by jumping straight to PLAY. A cutscene
// you cannot skip is a cutscene you resent on the second run, and this one
// is long.

#ifndef PM_SCREENS_H
#define PM_SCREENS_H

#include <m64/m64_widget.h>
#include <m64/m64_input.h>
#include <m64/m64_camera.h>
#include <m64/m64_fpscam.h>
#include <m64/m64_engine.h>

#include "pm_types.h"
#include "pm_veil.h"

#define PM_SAVE_SLOTS 3

// Mixer channel for the ambience bed. Held for the whole session rather
// than allocated per play: it is one looping sound that never stops, and a
// stolen channel would take the room tone out from under a scene.
#define PM_CH_DRONE 0

// Mixer channel for the theme's streamed form (pm_music.h). Fixed for the
// same reason as the drone, and played at priority 255 so the auto
// allocator in m64_sfx_play_ex — which only steals a channel of strictly
// lower priority — can never hand it to a footstep. The tracker form does
// not need a reservation: XM64 plays in the mixer's separate music range.
#define PM_CH_MUSIC 1

typedef enum {
    PM_SCREEN_BOOT = 0,   // fade up from black, no input
    PM_SCREEN_TITLE,      // skull + menu over the drone flyover
    PM_SCREEN_ATTRACT,    // the reel cycles; any button returns to TITLE
    PM_SCREEN_FILE,       // three profiles
    PM_SCREEN_LAB_CINE,   // Horner in the lab, then into his head
    PM_SCREEN_LAB,        // first person: read, explore, find the MRI
    PM_SCREEN_INTAKE,     // the machine takes him
    PM_SCREEN_SUB,        // the LOACH, rising toward the island
    PM_SCREEN_BEACH,      // the crash, the guards, the reveal
    PM_SCREEN_PLAY,
} PMScreen;

/** What one profile holds. Kept small on purpose: EEPROM 4k has 504 usable
 *  bytes and m64_save's `backup` flag doubles every slot, so three slots of
 *  this plus a 4-byte header is the budget (see m64_save.h). */
// Story bits in PMSaveData.flags.
//
// SEEN_REVEAL gates the attract reel. The centaur is the intro's payoff —
// the first sight of him is meant to be on the beach, killing someone —
// so the reel does NOT show him until a profile has got there. Before
// that the title cycles the island and the lab, neither of which gives
// anything away. Unlocking attract content on completion is what a 1998
// game did with its best asset, and it costs one bit.
//
// (The veil and the demons are a later surprise again, and appear nowhere
// in the intro or the reel at all — see pm_demo.c.)
#define PM_FLAG_SEEN_REVEAL 0x01

typedef struct {
    uint16_t progress;  // how far the story has got
    uint16_t deaths;
    uint32_t playtime;  // seconds
    uint8_t  flags;     // per-profile story bits
    uint8_t  _pad[3];
} PMSaveData;

typedef struct {
    PMScreen screen;
    float    screen_t;   // seconds on the current screen
    float    fade;       // 1 = black, 0 = clear; drives the BOOT fade and
                         //   every transition that hides a model load

    M64WidgetStyle style;
    M64Menu        menu;

    // File select.
    uint8_t    slot_used[PM_SAVE_SLOTS];
    int        slot;         // the profile being played
    PMSaveData save;
    uint8_t    save_ok;      // 0 when the console has no EEPROM: the file
                             //   screen still runs, but nothing persists

    // Attract reel.
    int   reel;              // index into pm_demo_reel
    float idle_t;            // seconds since the player last touched anything

    // Set when the player picks an empty profile, so the intro runs. A
    // filled profile clears it and goes straight to PLAY.
    uint8_t intro;

    // Borrowed from main(), which owns it — the playable lab and PLAY are
    // the same camera, and handing it over rather than keeping a second
    // one is what stops the two from disagreeing about where Horner is.
    M64FpsCam *fpscam;

    int   drone;      // m64_sfx handle for the ambience bed, -1 if missing
    float drone_vol;

    // 1 if ANY profile has finished the intro. Read across every slot at
    // boot, not just the loaded one: the reveal is spoiled per-console,
    // not per-file.
    uint8_t seen_reveal;
} PMApp;

void pm_app_init(PMApp *app, M64FpsCam *fpscam);

/** Advance the active screen one frame. Owns the camera, because what
 *  changes between screens IS the camera mode — same division of labour
 *  gg_screens_update documents. Returns the screen after the update. */
PMScreen pm_screens_update(PMApp *app, const M64Input *in, M64Camera *cam,
                           M64Scene *scene, PMVeil *veil, float dt);

/** Draw the active screen's 3D layer. Call inside the 3D pass. */
void pm_screens_draw3d(PMApp *app);

/** Draw the active screen's 2D layer. Call inside the GUI pass. */
void pm_screens_draw2d(PMApp *app, int screen_w, int screen_h);

#endif // PM_SCREENS_H
