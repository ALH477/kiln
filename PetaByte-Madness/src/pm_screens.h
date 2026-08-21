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
//     NARRATION ─► LAB_CINE ─► LAB ─► INTAKE ─► CREDITS ─► SUB ─► BEACH
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
//   * NARRATION / CREDITS are also PMDemoShots (pm_narration.c/pm_credits.c)
//     but draw nothing in the 3D pass — both expose their own *_draw2d,
//     called directly below, the same way LAB exposes pm_lab_draw2d.
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

#include <kiln/kiln_widget.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_camera.h>
#include <kiln/kiln_fpscam.h>
#include <kiln/kiln_engine.h>

#include "pm_types.h"
#include "pm_veil.h"

#define PM_SAVE_SLOTS 3

// Mixer channel for the ambience bed. Held for the whole session rather
// than allocated per play: it is one looping sound that never stops, and a
// stolen channel would take the room tone out from under a scene.
#define PM_CH_DRONE 0

// Mixer channel for the theme's streamed form (pm_music.h). Fixed for the
// same reason as the drone, and played at priority 255 so the auto
// allocator in kiln_sfx_play_ex — which only steals a channel of strictly
// lower priority — can never hand it to a footstep. The tracker form does
// not need a reservation: XM64 plays in the mixer's separate music range.
#define PM_CH_MUSIC 1

// Mixer channel for the two story-beat music cues: the narration crawl's
// score (pm_narration.c) and the surgery OST (pm_lab.c, spanning LAB and
// INTAKE). Fixed and shared between them, same priority-255 reservation as
// PM_CH_DRONE/PM_CH_MUSIC above — safe because the two never overlap: the
// narration cue's own teardown (pm_demo_stop, called from inside
// pm_demo_play before LAB_CINE's setup runs) always stops it two screens
// before the surgery cue ever starts.
#define PM_CH_STORY 2

// ── The screen list ────────────────────────────────────────────────────
// One list, three consumers: the PMScreen enum below, pm_debug.c's overlay
// labels, and pm_screens.c's ENTER_FN jump table (which the debug boot menu
// walks).
//
// It is an X-macro rather than an enum plus a hand-kept name array because
// the hand-kept version drifted, twice, and both times silently.
// PM_SCREEN_NARRATION and PM_SCREEN_CREDITS landed in the enum with no
// matching entry in pm_debug.c's array, so every label from "LAB_CINE" on
// reported the PREVIOUS screen's name — the bounds check there guarded only
// against running off the end of a too-short array, not against the array
// being internally misaligned with the enum it labels. The identical bug in
// the same file's MODEL_NAMES was a NULL format string on the first frame
// the overlay ran, i.e. a boot crash on every build. Deriving both from one
// list is what makes that class of defect unrepresentable rather than
// merely fixed.
//
//   X(id, short_name)
// short_name is what the debug overlay prints; keep it to six characters or
// so, because the overlay fits the screen name, the timer, the fade and the
// frame rate onto one 320 px line.
//
// Screen-by-screen:
//   BOOT       fade up from black, no input
//   TITLE      skull + menu over the drone flyover
//   ATTRACT    the reel cycles; any button returns to TITLE
//   FILE       three profiles
//   NARRATION  the backstory crawl, on an empty slot only
//   LAB_CINE   Horner in the lab, then into his head
//   LAB        first person: read, explore, find the MRI
//   INTAKE     the machine takes him
//   CREDITS    title card + FMV, before the beach
//   SUB        the LOACH, rising toward the island
//   BEACH      the crash, the guards, the reveal
//   PLAY       the corridor
#define PM_SCREEN_LIST(X)          \
    X(PM_SCREEN_BOOT,      "BOOT")      \
    X(PM_SCREEN_TITLE,     "TITLE")     \
    X(PM_SCREEN_ATTRACT,   "ATTRACT")   \
    X(PM_SCREEN_FILE,      "FILE")      \
    X(PM_SCREEN_NARRATION, "NARRATION") \
    X(PM_SCREEN_LAB_CINE,  "LAB_CINE")  \
    X(PM_SCREEN_LAB,       "LAB")       \
    X(PM_SCREEN_INTAKE,    "INTAKE")    \
    X(PM_SCREEN_CREDITS,   "CREDITS")   \
    X(PM_SCREEN_SUB,       "SUB")       \
    X(PM_SCREEN_BEACH,     "BEACH")     \
    X(PM_SCREEN_PLAY,      "PLAY")

typedef enum {
#define PM_SCREEN_ENUM(id, name) id,
    PM_SCREEN_LIST(PM_SCREEN_ENUM)
#undef PM_SCREEN_ENUM
    PM_SCREEN_COUNT,
} PMScreen;

/** The overlay label for a screen id, or "?" out of range. Defined in
 *  pm_screens.c (not pm_debug.c) so it exists in the shipping ROM too —
 *  the debug boot menu is the other caller, and both want the same names. */
const char *pm_screen_name(PMScreen s);

/** What one profile holds. Kept small on purpose: EEPROM 4k has 504 usable
 *  bytes and kiln_save's `backup` flag doubles every slot, so three slots of
 *  this plus a 4-byte header is the budget (see kiln_save.h). */
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

/** A deferred, possibly-blocking transition action — see go_fade's comment
 *  in pm_screens.c for why this exists rather than running inline. */
struct PMApp;  // forward declaration: a tag named only inside the parameter
               // list below would get function-prototype scope (its own,
               // separate `struct PMApp`, incompatible with the real one) —
               // this file-scope declaration is what makes it the same tag.
typedef void (*PMTransitionFn)(struct PMApp *app);

typedef struct PMApp {
    PMScreen screen;
    float    screen_t;   // seconds on the current screen
    float    fade;       // 1 = black, 0 = clear; drives the BOOT fade and
                         //   every transition that hides a model load

    // A transition armed by go_fade(), not yet resolved. go_fade() does NOT
    // switch `screen` itself — it snaps `fade` to 1.0 and stashes these, and
    // the NEXT pm_screens_update() call (i.e. only once THIS frame's solid
    // black has actually been drawn and presented) runs pending_fn and then
    // performs the switch. See go_fade's comment for the bug this fixes.
    PMTransitionFn pending_fn;
    PMScreen       pending_screen;
    uint8_t        pending;

    KilnWidgetStyle style;
    KilnMenu        menu;

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
    KilnFpsCam *fpscam;

    int   drone;      // kiln_sfx handle for the ambience bed, -1 if missing
    float drone_vol;

    // 1 if ANY profile has finished the intro. Read across every slot at
    // boot, not just the loaded one: the reveal is spoiled per-console,
    // not per-file.
    uint8_t seen_reveal;
} PMApp;

void pm_app_init(PMApp *app, KilnFpsCam *fpscam);

/** Advance the active screen one frame. Owns the camera, because what
 *  changes between screens IS the camera mode — same division of labour
 *  gg_screens_update documents. Returns the screen after the update. */
PMScreen pm_screens_update(PMApp *app, const KilnInput *in, KilnCamera *cam,
                           KilnScene *scene, PMVeil *veil, float dt);

/** Draw the active screen's 3D layer. Call inside the 3D pass. */
void pm_screens_draw3d(PMApp *app);

/** Draw the active screen's 2D layer. Call inside the GUI pass. */
void pm_screens_draw2d(PMApp *app, int screen_w, int screen_h);

#endif // PM_SCREENS_H
