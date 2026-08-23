// SPDX-License-Identifier: MIT
//
// kiln_camlint.h — static validation of a keyframed camera shot.
//
// Originated in a downstream game (now in that game's own repo, which keeps
// a shim over its old header path) and moved into the engine alongside
// kiln_camkey when Forge's CAM mode needed it: an editor that lets you SAVE a
// table the game will then refuse is worse than one that never validated,
// because the failure surfaces two tools later. So Forge runs the same rules
// at authoring time.
//
// Moving it cost nothing structurally, because it was already written to be
// movable: the originating game's own shot struct deliberately MIRRORED the
// fields of its runtime's camera-table struct rather than taking one,
// precisely so this header needed nothing from the game. That discipline is
// what made a second consumer a rename rather than a rewrite.
//
// Every camera bug that game had was a static property of a keyframe table
// sitting next to a dimension its generators already published, and every
// one was found by building a ROM and looking at it: an eye behind a wall,
// an eye inside a landmark's own footprint rendering black, an orbit chord
// dipping through terrain, and a frustum default that cost several shots
// their geometry.
//
// None of those needed an emulator to find. They needed someone to compare two
// numbers.
//
// ── Hard failures and notes are different kinds of thing ────────────────
// The ERR_ flags are facts: the shot is wrong in a way that does not depend on
// taste, intent or what it is pointed at. A key whose time is past the shot's
// duration is unreachable — a runtime that clamps elapsed time at `duration`
// never reaches it — so it is dead data whatever it says. An eye equal to
// its look target is a zero-length view vector, which normalises to a NaN
// and halts the VR4300 (a real crash this has actually produced).
//
// The NOTE_ flags are measurements. Every one of them is legitimately
// intentional in some shot: a wide exterior shot's eye IS outside the room, and
// a whip-pan IS a speed spike. Failing a build on them would mean writing
// waivers for correct work, which teaches people to write waivers. So they are
// measured, printed, and never fatal.
//
// ── Where this runs ─────────────────────────────────────────────────────
// Two places, and the split is deliberate.
//
//   * NATIVELY, in `nix flake check` (nix/checks/kiln-logic.nix), against
//     synthetic tables — one clean, and one deliberately broken per ERR_ flag.
//     That verifies the DETECTOR, in both directions, in seconds, with no
//     emulator. A gate only ever seen to pass is a gate that might not be
//     checking anything.
//
//   * ON CONSOLE, over a game's real tables, in a debug ROM built for the
//     purpose. Real keyframe tables often cannot leave the ROM build at all —
//     a runtime's own camera code needs libdragon and Tiny3D, and a table
//     built procedurally at boot is not even a compile-time constant — so
//     the data is inspected where it lives.
//
// This file is therefore FREE OF libdragon and Tiny3D by construction: it
// includes kiln_camkey.h, whose only dependency is `fm_vec3_t`. Keep it that way,
// or the host half stops compiling and the detector stops being tested.

#ifndef KILN_CAMLINT_H
#define KILN_CAMLINT_H

#include <stdint.h>

#include "kiln_camkey.h"

// ── Hard failures ──────────────────────────────────────────────────────
#define KILN_CAMLINT_ERR_NO_KEYS      0x0001u  /* key_count 0, or a NULL table      */
#define KILN_CAMLINT_ERR_TIME_ORDER   0x0002u  /* keys not strictly non-decreasing  */
#define KILN_CAMLINT_ERR_KEY_PAST_END 0x0004u  /* a key at t > duration: dead data  */
#define KILN_CAMLINT_ERR_DEGENERATE   0x0008u  /* eye == look: the NaN view vector  */
#define KILN_CAMLINT_ERR_FRUSTUM      0x0010u  /* near < 0, or far <= near          */
#define KILN_CAMLINT_ERR_SUBJECT_CUT  0x0020u  /* |look-eye| > far_z                */
#define KILN_CAMLINT_ERR_NAN          0x0040u  /* a NaN or infinity anywhere        */
#define KILN_CAMLINT_ERR_COUNT        7

// ── Measurements ───────────────────────────────────────────────────────
#define KILN_CAMLINT_NOTE_LATE_START  0x0001u  /* keys[0].t != 0                    */
#define KILN_CAMLINT_NOTE_DEAD_TAIL   0x0002u  /* long hold past the last key       */
#define KILN_CAMLINT_NOTE_OVERSHOOT   0x0004u  /* the spline bulges past its chord  */
#define KILN_CAMLINT_NOTE_HITCH       0x0008u  /* one segment far faster than another*/
#define KILN_CAMLINT_NOTE_OUTSIDE     0x0010u  /* an eye outside the supplied bounds */
#define KILN_CAMLINT_NOTE_NEAR_AIM    0x0020u  /* look target inside the near plane  */

// ── Thresholds ─────────────────────────────────────────────────────────
// Both are SCALE-FREE on purpose. This world runs at 64 units to the metre and
// the shots it has to cover span three orders of magnitude — a face at arm's
// length in the lab and an island 13,000 units across — so an absolute
// tolerance would be meaningless on one end or the other.

/** Overshoot as a fraction of the segment's own chord length. A curve bulging
 *  a quarter of the chord past the straight line between its keys is a visible
 *  swing at any scale. */
#define KILN_CAMLINT_OVERSHOOT_FRAC   0.25f

/** Fastest segment / slowest segment, in units per second. Catmull-Rom removed
 *  the per-key deceleration; this catches the other half of the same problem,
 *  where the keys themselves are spaced so unevenly that the shot lurches. */
#define KILN_CAMLINT_HITCH_RATIO      8.0f

/** Hold past the last key, as a fraction of the shot's duration. */
#define KILN_CAMLINT_TAIL_FRAC        0.25f

/** Samples per segment when measuring the flown curve. 16 puts the sample
 *  spacing well under the bulge being measured without making the validator
 *  something you would think twice about running. */
#define KILN_CAMLINT_SAMPLES_PER_SEG  16

/** Optional containment volume — the room a shot is supposed to stay inside.
 *  `valid = 0` skips the check entirely, which is what every exterior shot
 *  wants. */
typedef struct {
    fm_vec3_t mins, maxs;
    int       valid;
} KilnCamBounds;

/** What to validate. Mirrors the fields of a game's own camera-shot struct
 *  rather than taking one, so this header stays free of that game's runtime
 *  headers (and therefore of libdragon). */
typedef struct {
    const KilnCamKey *keys;
    int   key_count;
    float duration;
    float near_z, far_z;   /**< RESOLVED, not raw: pass the values the shot will
                            *   actually run with, i.e. PM_SHOT_NEAR_Z /
                            *   PM_SHOT_FAR_Z already substituted for a shot
                            *   that declared none. Validating the raw 0 would
                            *   check a value the renderer never sees. */
    int   loop;
} KilnCamShot;

typedef struct {
    uint32_t err;            /**< KILN_CAMLINT_ERR_* — nonzero means broken       */
    uint32_t note;           /**< KILN_CAMLINT_NOTE_* — measured, never fatal     */

    int   bad_key;           /**< first key index implicated by `err`, or -1 */

    float overshoot;         /**< worst bulge, as a fraction of chord length */
    int   overshoot_seg;
    float speed_max, speed_min, speed_ratio;   /**< units/sec between keys   */
    float subject_dist;      /**< the largest |look - eye| over all keys     */
    float tail;              /**< seconds between the last key and duration  */
    int   outside_key;       /**< first eye outside `bounds`, or -1          */
} KilnCamReport;

/** Validate `shot`. `bounds` may be NULL (or have `valid = 0`). `out` may be
 *  NULL. Returns the hard-failure mask, so `if (kiln_camlint(...))` reads
 *  correctly. */
uint32_t kiln_camlint(const KilnCamShot *shot, const KilnCamBounds *bounds,
                      KilnCamReport *out);

/** Short, stable name for a single ERR_ or NOTE_ bit — for the on-console
 *  report and the host check's failure messages. Returns "?" for a mask with
 *  more or fewer than one bit set. */
const char *kiln_camlint_err_name(uint32_t one_bit);
const char *kiln_camlint_note_name(uint32_t one_bit);

#endif // KILN_CAMLINT_H
