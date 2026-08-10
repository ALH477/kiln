// SPDX-License-Identifier: MPL-2.0

#include "gg_buds.h"

void gg_buds_add(GGPlayer *p, int delta)
{
    p->buds += delta;
    if (p->buds < 0) p->buds = 0;
}

int gg_buds_total(const GGPlayer *players, int n)
{
    int t = 0;
    for (int i = 0; i < n; i++) t += players[i].buds;
    return t;
}