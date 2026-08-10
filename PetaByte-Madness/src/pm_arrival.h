// SPDX-License-Identifier: MPL-2.0
//
// pm_arrival.h — the submarine, the beach, and the reveal.
//
// Two shots that close the intro. The machine put Horner in the LOACH
// while he was under; he wakes up arriving.
//
// ── The sub is NOT a drone flyover ─────────────────────────────────────
// The flyover belongs to the main menu. This one is low and rising: it
// starts on the seabed looking up, the hull crosses overhead, the surface
// brightens, and the island resolves ahead. If the two ever converge the
// arrival stops being an arrival and becomes the title screen again.
//
// ── The beach is the reveal ────────────────────────────────────────────
// The sub hits the sand. Two guards are standing where it lands, and they
// have no idea what is inside it. He climbs out and kills both of them in
// about four seconds — `notice`, `fire`, `slash`, `shout` — and the camera
// drives into his face and cuts to binary.
//
// This is the first time the player sees the centaur, and it is deliberate
// that he sees it doing this. You never saw Horner's body in the lab (that
// section is first person), the sub pan stays outside the hull, and so the
// first look at what he became is the thing killing two people with it.
//
// It is also the only place in the game so far where four of the rig's
// thirteen animations run back to back, which makes it the real test of
// the whole Phase 1 conversion (docs/ASSET_PIPELINE.md).

#ifndef PM_ARRIVAL_H
#define PM_ARRIVAL_H

#include "pm_demo.h"

/** The submarine pan: seabed -> up past the LOACH -> surface -> island. */
extern const PMDemoShot pm_arrival_sub;

/** The beach: crash, two guards, climb out, fire, slash, shout, and into
 *  his face. Ends on the binary. */
extern const PMDemoShot pm_arrival_beach;

/** Free the skeletons the beach allocated. Safe to call twice. */
void pm_arrival_close(void);

#endif // PM_ARRIVAL_H
