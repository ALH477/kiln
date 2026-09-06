/* SPDX-License-Identifier: MIT
 *
 * Loads a real Quake .map through the real kiln_map.c, off the host VFS, and
 * renders it — the first time a level in this repo has been drawn anywhere but
 * a ROM.
 *
 * This is the payoff for the IO tier. kiln_map_load takes a DFS path, which on
 * console reads a filesystem image appended to the ROM; here it resolves under
 * $KILN_HOST_DFS. The chain that matters is the one CLAUDE.md says cost
 * PetaByte Madness its entire PLAY screen: an asset builder's `name` IS the
 * filename the ROM opens, kiln_map_load returns non-zero instead of asserting,
 * and an empty clip world makes every trace report "nothing in the way". Every
 * layer degrades politely and the composition is silent. On the host that same
 * mismatch is a missing file with a path in the message.
 *
 * It also asserts the brush/face counts, because a .map parse that silently
 * drops geometry renders as level-design intent.
 *
 * ── This file no longer draws ───────────────────────────────────────────
 * The scene, the frame and the capture moved to tools/maprender/map_render.c
 * so that `./dev map-render` — which an author or an agent points at arbitrary
 * content — draws THE SAME FRAME this gate compares. What is left here is only
 * what is specific to assets/quake_test.map: the absent-map probe, and the
 * assertions that pin kiln_map.c's brush reduction. A flag on a shared body
 * would have kept those assertions inside the tool, where they are noise on
 * every other map; a split makes the sharing structural.
 *
 * ── This check used to pin two DEFECTS. It now pins their fix ───────────
 * Rendering a .map on the host is what made both visible, and both are fixed:
 *
 *   1. kiln_map_draw did not render the brush's FACES. A Quake .map gives
 *      three points per face and those points define a PLANE — conventionally
 *      one unit apart, which is exactly what assets/quake_test.map uses.
 *      kiln_map.c read them as face corners and completed the parallelogram
 *      p0,p1,p2,p0+p2-p1, so a 128-unit wall rendered as a 1x2-unit patch at
 *      one corner, six per brush. This check measured it and said so:
 *      "face 0 spans 2 units on y; the brush spans 129".
 *
 *      kiln_map.c now does real brush CSG — every triple of planes
 *      intersected, candidates kept only if inside all the others, survivors
 *      wound into a ring — ported from tools/blender/quake_map.py, which has
 *      had the correct algorithm on the host side all along.
 *
 *   2. t3d_vert_pack_normal's uint16_t was assigned into a uint8_t, discarding
 *      the whole x field and half of y. Three of this brush's six faces came
 *      out with normA == 0, i.e. no normal at all.
 *
 * Every assertion below therefore reads the opposite way from the way it read
 * before, and the numbers are the authored ones rather than the convention
 * leaking through: 128 units, not 2; zero zeroed normals, not three; and a
 * collision box of -64..64, not -64..65.
 *
 * The AABB is the part worth a second look, because it is the field every
 * consumer actually uses — kiln_clip_set_world, kiln_room's brush install, a
 * game's PLAY screen — and until now it was the min/max of the plane POINTS,
 * so every collision box in every level here was a unit oversized on whichever
 * axes the plane-point convention pushed outward. Real vertices make it the
 * real box. kiln_map.c keeps the plane-point box as a fallback for exactly one
 * case, an inside-out brush that yields no vertices at all, so that a winding
 * problem stays a rendering problem instead of becoming a brush with no
 * collision; assets/quake_test.map is deliberately not canonicalised but it is
 * wound correctly, so it does not take that path.
 */
#include "map_render.h"

#include <kiln_map.h>
#include <kiln_clip.h>
#include <kiln_host.h>

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(int argc, char **argv)
{
    const char *map = argc > 1 ? argv[1] : "rom:/quake_test.map";
    const char *png = argc > 2 ? argv[2] : "map.png";

    map_render_init();

    /* A missing map must be a LOUD miss, not a silent empty world. Prove the
     * negative first, because it is the failure mode that actually happened. */
    KilnMap absent;
    memset(&absent, 0, sizeof absent);
    CHECK(kiln_map_load(&absent, "rom:/definitely_not_here.map") != 0,
          "loading a nonexistent map returned success");
    CHECK(absent.brush_count == 0, "a failed load left %u brushes",
          absent.brush_count);

    KilnMap m;
    memset(&m, 0, sizeof m);
    const int rc = map_render_open(&m, map);
    CHECK(rc == 0, "kiln_map_load('%s') returned %d", map, rc);
    if (rc != 0) return 1;

    /* ── The brush reduction, pinned ─────────────────────────────────
     * See the file header: these three assertions used to pin two defects and
     * now pin their fix. Each still prints its measurement before judging it,
     * because a bare pass/fail on geometry tells you nothing about which way
     * it went wrong the day it breaks.
     */
    CHECK(m.brush_count == 1, "%u brushes, expected 1", m.brush_count);
    CHECK(m.face_count == m.brush_count * 6,
          "%u faces for %u brushes; the CSG emits one polygon per plane that "
          "reaches the surface, and an axial cube's six planes all do, so this "
          "should be exactly 6x", m.face_count, m.brush_count);

    /* The AABB is now the min/max of the brush's real vertices, not of its
     * plane points, so it is the box the level author drew. -64..65 here means
     * the plane-point convention is leaking into the collision box again. */
    CHECK(m.world_aabb_min.v[0] == -64.0f && m.world_aabb_max.v[0] == 64.0f,
          "world aabb x is %.0f..%.0f, expected -64..64 — this brush is "
          "authored -64..64 and the AABB comes from its CSG vertices",
          (double)m.world_aabb_min.v[0], (double)m.world_aabb_max.v[0]);
    CHECK(m.world_aabb_min.v[1] == -64.0f && m.world_aabb_max.v[1] == 64.0f,
          "world aabb y is %.0f..%.0f, expected -64..64",
          (double)m.world_aabb_min.v[1], (double)m.world_aabb_max.v[1]);
    CHECK(m.world_aabb_min.v[2] == -64.0f && m.world_aabb_max.v[2] == 64.0f,
          "world aabb z is %.0f..%.0f, expected -64..64",
          (double)m.world_aabb_min.v[2], (double)m.world_aabb_max.v[2]);

    {
        /* Finding 1, measured the same way it was measured when it was a
         * defect: the face's own extent against the brush's. A cube face spans
         * the whole brush on the two axes it is not normal to. */
        CHECK(m.faces[0].vert_count == 4,
              "face 0 has %u vertices, expected 4 — a cube face is a quad, and "
              "a count of 3 means the CSG lost a corner to an epsilon",
              m.faces[0].vert_count);

        const T3DVertPacked *v = m.faces[0].verts;
        int lo = 32767, hi = -32768;
        for (uint8_t i = 0; i < m.faces[0].vert_count; i++) {
            const int y = (i & 1) ? v[i / 2].posB[1] : v[i / 2].posA[1];
            if (y < lo) lo = y;
            if (y > hi) hi = y;
        }
        printf("  face 0 spans %d units on y; the brush spans %.0f\n",
               hi - lo, (double)(m.world_aabb_max.v[1] - m.world_aabb_min.v[1]));
        CHECK(hi - lo == 128,
              "face 0 spans %d units on y, expected the brush's own 128. A "
              "span of 2 is the old parallelogram-from-three-plane-points "
              "reduction coming back", hi - lo);
    }
    {
        /* Finding 2, measured: normals truncated to a byte lost the x field
         * entirely, so the three faces whose normal is +/-X arrived as 0. */
        int zeroed = 0;
        for (uint16_t i = 0; i < m.face_count; i++)
            if (m.faces[i].verts[0].normA == 0) zeroed++;
        printf("  %d of %u face normals are zero\n", zeroed, m.face_count);
        CHECK(zeroed == 0,
              "%d of %u face normals are zero — t3d_vert_pack_normal returns a "
              "uint16_t and storing it in a uint8_t drops the x field",
              zeroed, m.face_count);

        /* A packed normal is 5.6.5 signed, so a face pointing along -X carries
         * a non-zero value in the TOP five bits: the half a byte discarded.
         * Asserting only "not zero" would pass on a normal whose x survived by
         * luck and whose z did not. */
        int with_x = 0;
        for (uint16_t i = 0; i < m.face_count; i++)
            if ((m.faces[i].verts[0].normA >> 11) != 0) with_x++;
        CHECK(with_x == 2,
              "%d of %u face normals carry an x component, expected 2 — an "
              "axial cube has exactly one +X face and one -X face",
              with_x, m.face_count);
    }

    CHECK(map_render_frame(&m, png) == 0, "could not write %s", png);

    CHECK(kiln_clip_world_count() == m.brush_count,
          "clip world has %d brushes, map has %u", kiln_clip_world_count(),
          m.brush_count);

    const KilnHostT3DCounters *t = kiln_host_t3d_counters();
    CHECK(t->tris_submitted > 0, "no geometry submitted");
    CHECK(t->tris_drawn > 0, "geometry submitted but nothing rasterised");

    kiln_map_free(&m);

    if (fails) { printf("\nFAILED (%d)\n", fails); return 1; }
    printf("a real .map loaded off the host VFS and rendered\n");
    return 0;
}
