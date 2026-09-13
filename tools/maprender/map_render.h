/* SPDX-License-Identifier: MIT
 *
 * map_render.h — load a Quake .map through the real kiln_map.c and draw one
 * frame of it with the real engine, on the host.
 *
 * This body was extracted verbatim from nix/checks/kiln-map-check.c so that
 * `./dev map-render` and that check draw THE SAME FRAME. They compare against
 * the same nix/checks/refs/kiln-map.png and there is never a second reference
 * image — see AGENTS.md, "bless a per-architecture reference image ... defeats
 * the point".
 *
 * ── The rule this header exists to state ────────────────────────────────
 * ALL DRAWING LIVES HERE. Neither consumer's main() may call a kiln_gui_* or
 * kiln_dd_* primitive of its own. The moment a caller draws, the tool's frame
 * and the gate's reference are the same image only by accident, and the tool
 * stops being evidence about the engine.
 *
 * That is not a hypothetical: tools/uipreview used to implement kiln_gui's
 * primitives itself, and disagreed with the console about panel edge order,
 * bar inset, and whether kiln_gui_rect blends alpha at all. nix/checks/
 * kiln-widget.nix exists because of it. This is the same lesson one layer
 * down, written before it can happen again rather than after.
 */
#ifndef KILN_MAP_RENDER_H
#define KILN_MAP_RENDER_H

#include <kiln_map.h>

/** Bring up the engine, the 2D pass and the host VFS. Call once, first. */
void map_render_init(void);

/** Load `dfs_path` and print the counts and the world AABB. Returns
 *  kiln_map_load's own return code: 0 on success, non-zero on any I/O or
 *  parse failure — deliberately NOT an assert, because a missing map must be
 *  a loud miss rather than a silent empty world. */
int  map_render_open(KilnMap *out, const char *dfs_path);

/** Install the brushes into the clip world, draw one frame, print the T3D
 *  counters and kiln_host_stats, and write `png`. Returns 0 on success. */
int  map_render_frame(const KilnMap *m, const char *png);

#endif /* KILN_MAP_RENDER_H */
