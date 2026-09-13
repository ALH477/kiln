/* SPDX-License-Identifier: MIT
 *
 * maprender — `./dev map-render <file.map> [out.png]`.
 *
 * Load a level through the real kiln_map.c and draw it with the real engine,
 * with no ROM, no emulator and no Wayland session. `./dev shot` needs Ares on
 * a live compositor; this needs nothing, which is what makes it reachable from
 * a build script, a test, or an agent.
 *
 * It draws nothing itself. The frame comes from tools/maprender/map_render.c,
 * which nix/checks/kiln-map.nix compiles too, so what you look at here is what
 * the gate compares. See map_render.h for why that rule is written down.
 */
#include "map_render.h"

#include <kiln_host.h>
#include <t3d/t3d.h>

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: maprender <file.map> [out.png]\n");
        return 2;
    }
    const char *map = argv[1];
    const char *png = argc > 2 ? argv[2] : "map.png";

    map_render_init();

    KilnMap m;
    memset(&m, 0, sizeof m);
    if (map_render_open(&m, map) != 0) {
        /* kiln_map_load's own failure, surfaced with the path. On console this
         * is the silent case CLAUDE.md says cost a whole PLAY screen: the load
         * returns non-zero, the clip world stays empty, and every trace reports
         * "nothing in the way". Here it is one line with the name in it. */
        fprintf(stderr, "maprender: could not load '%s'\n", map);
        fprintf(stderr, "  the path is resolved under $KILN_HOST_DFS; a .map "
                        "that fails to PARSE fails here too.\n");
        fprintf(stderr, "  run './dev map-validate %s' to find out which.\n", map);
        return 1;
    }

    if (map_render_frame(&m, png) != 0) {
        fprintf(stderr, "maprender: could not write %s\n", png);
        return 1;
    }

    /* The two things kiln_host_stats does not say, and the most useful lines
     * for anyone who cannot look at the PNG.
     *
     * "submitted > 0 but drawn == 0" is the everything-is-behind-the-camera
     * signature, and it is invisible in a colour histogram: the frame is a
     * clean, plausible, entirely empty room. That exact shape cost this repo
     * four bugs in examples/openworld-demo, one of which was a draw callback
     * subtracting the camera position twice and pushing every tile off the far
     * plane. A histogram called that frame "mostly background" and was right. */
    const KilnHostT3DCounters *t = kiln_host_t3d_counters();
    if (t->tris_submitted > 0 && t->tris_drawn == 0)
        printf("WARNING: %u triangles submitted, NONE rasterised — the camera "
               "is probably not looking at the geometry\n", t->tris_submitted);
    else if (kiln_host_counters()->shaded_px == 0)
        printf("WARNING: no pixels were written; the frame is empty\n");

    printf("wrote %s\n", png);
    kiln_map_free(&m);
    return 0;
}
