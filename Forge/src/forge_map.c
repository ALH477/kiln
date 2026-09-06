/* SPDX-License-Identifier: MIT
 *
 * forge_map.c — see forge_map.h.
 */
#include "forge_map.h"
#include "forge_vocab.gen.h"

#include <stdio.h>

int forge_map_emit_box(char *out, size_t cap, const int mins[3],
                       const int maxs[3], const char *tex)
{
    int n = snprintf(out, cap, "{\n");

    /* A loop over the GENERATED corner selector, not over axes. forge_io.c's
     * standing warning — "do not tidy these into a loop over axes without
     * re-deriving the winding: the outward normal is cross(p3-p1, p2-p1)" —
     * was protecting a hand-transcribed table. The table is now data
     * (tools/schema/level_vocab.json), so this loop cannot re-derive it
     * wrongly; it can only reproduce it. */
    for (int f = 0; f < FORGE_VOCAB_FACE_COUNT; f++) {
        int p[3][3];
        for (int pt = 0; pt < 3; pt++)
            forge_vocab_aabb_corner(f, pt, mins, maxs, p[pt]);
        n += snprintf(out + n, (n < (int)cap) ? cap - (size_t)n : 0,
                      "( %d %d %d ) ( %d %d %d ) ( %d %d %d ) %s 0 0 0 1 1\n",
                      p[0][0], p[0][1], p[0][2],
                      p[1][0], p[1][1], p[1][2],
                      p[2][0], p[2][1], p[2][2], tex);
    }

    n += snprintf(out + n, (n < (int)cap) ? cap - (size_t)n : 0, "}\n");
    return n;
}
