/* SPDX-License-Identifier: MIT */
#include "dungeon_view.h"

int dungeon_view_runs(const uint8_t *chunk, DungeonRun *runs)
{
    int n = 0;
    for (int y = 0; y < 64; y++) {
        int x = 0;
        while (x < 64) {
            const uint8_t k = chunk[y * 64 + x];
            if (k == 0) { x++; continue; }
            int e = x + 1;
            while (e < 64 && chunk[y * 64 + e] == k) e++;
            runs[n].x = (uint8_t)x;
            runs[n].y = (uint8_t)y;
            runs[n].w = (uint8_t)(e - x);
            runs[n].kind = k;
            n++;
            x = e;
        }
    }
    return n;
}

void dungeon_view_census(const uint8_t *chunk, uint32_t counts[4])
{
    counts[0] = counts[1] = counts[2] = counts[3] = 0;
    for (int i = 0; i < 4096; i++)
        if (chunk[i] < 4) counts[chunk[i]]++;
}
