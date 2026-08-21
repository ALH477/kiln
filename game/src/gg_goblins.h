// SPDX-License-Identifier: MPL-2.0
//
// gg_goblins.h — the four playable Ganja Goblin characters. Each has a
// unique passive + charged special, per the design doc:
//
//   Dank    — chill.     Passive: +1 bud per Grow space.  Special: Couch Lock.
//   Sparky  — hyper.     Passive: +1 movement every 3rd turn. Special: Spark Plug.
//   Moss    — sticky.    Passive: items last +1 turn.       Special: Resin Trap.
//   Glimmer — lucky.     Passive: +1 to dice when behind.   Special: Lucky Puff.
//
// The passives + specials are implemented in gg_specials.c; this file is
// just the table that wires them to kiln_char profile structs.

#ifndef GG_GOBLINS_H
#define GG_GOBLINS_H

#include <kiln/kiln_char.h>

#define GG_GOBLIN_COUNT 4

typedef enum {
    GG_GOBLIN_DANK    = 0,
    GG_GOBLIN_SPARKY  = 1,
    GG_GOBLIN_MOSS    = 2,
    GG_GOBLIN_GLIMMER = 3,
} GGGoblin;

extern const KilnCharProfile gg_goblins[GG_GOBLIN_COUNT];

#endif // GG_GOBLINS_H