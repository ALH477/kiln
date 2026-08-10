// SPDX-License-Identifier: MPL-2.0
//
// gg_buds.h — bud counter helpers. Buds are Ganja Goblin's currency/score,
// kept per-player in GGPlayer.buds. Phase 2 just adds and clamps; Phase 5's
// status effects (Munchies force-feed, Spore Cloud scramble) layer on top.

#ifndef GG_BUDS_H
#define GG_BUDS_H

#include "gg_types.h"

void gg_buds_add(GGPlayer *p, int delta);
int  gg_buds_total(const GGPlayer *players, int n);

#endif // GG_BUDS_H