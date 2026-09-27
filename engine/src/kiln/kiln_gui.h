/* SPDX-License-Identifier: MIT
 *
 * kiln_gui.h — the 2D half of Figulina.
 *
 * An immediate-mode overlay drawn after the 3D pass: panels, text, bars,
 * gauges. Screen coordinates, top-left origin, no depth, no lighting.
 *
 * ── Why immediate mode ─────────────────────────────────────────────────
 * A retained widget tree buys you layout and hit-testing, and costs you a
 * per-widget allocation plus a tree walk every frame. On a 93.75 MHz VR4300
 * with 4 MB of RAM, for a HUD that is a dozen rectangles and some text, that
 * trade is the wrong way round — you call the draw functions you want, in
 * order, and nothing is retained between frames.
 *
 * (This is the opposite choice from DeMoD UI's retained DmWidget tree, which
 * is right for a desktop-class panel app with encoder navigation and wrong
 * here. Different machine, different answer.)
 *
 * ── State ──────────────────────────────────────────────────────────────
 * fig_gui_begin() performs the one RDP state transition from the 3D pass:
 * depth test off, standard combiner, alpha blending on. Every widget below
 * assumes that has happened. Calling them inside the 3D pass will produce
 * depth-tested, possibly occluded 2D — which is exactly the bug the explicit
 * bracket exists to make obvious.
 */
#ifndef FIG_GUI_H
#define FIG_GUI_H


/* The prefix migration train (docs/NAMING.md section 9 step 2). Pulled in by
 * every public header (a quoted include, so it resolves both in this tree and
 * in the installed include/kiln prefix) rather than force-included by
 * kiln-inst.mk, because a
 * force-include only reaches builds that include that file — a Nix check or a
 * host build compiling a downstream's sources directly never saw it, and
 * PetaByte-Madness' pm-cine check is what proved that. Deleting the train is
 * still a scripted one-line removal from these headers plus the file itself.
 */
#include "kiln_compat.h"

#include <libdragon.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Font slot used for the built-in monospace debug font. Registered by
 *  fig_gui_init(); needs no font asset in the filesystem.
 *
 *  fig_engine_init() ALREADY CALLS THIS. Calling it again is safe (the
 *  function is idempotent) but unnecessary — a ROM only needs it if it
 *  brought up display/rdpq/t3d by hand instead of via fig_engine_init. */
#define FIG_GUI_FONT 1

/** Called by fig_engine_init(). Registers the built-in font. */
void fig_gui_init(void);
void fig_gui_close(void);

/** Bracket the 2D pass. Must be called after the 3D pass, inside the same
 *  fig_frame_begin/fig_frame_end. */
void fig_gui_begin(void);
void fig_gui_end(void);

/** Solid filled rectangle. */
void fig_gui_rect(int x, int y, int w, int h, color_t c);

/** Panel: filled body plus a 1px border. The workhorse for HUD boxes. */
void fig_gui_panel(int x, int y, int w, int h, color_t fill, color_t border);

/** Text at a baseline position, printf-style. */
void fig_gui_text(int x, int y, color_t c, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/** Horizontal progress/meter bar. `frac` is clamped to [0,1]. */
void fig_gui_bar(int x, int y, int w, int h, float frac, color_t fg, color_t bg);

/** Straight line of `thickness` pixels between two screen points.
 *
 *  Drawn as two shaded triangles rather than a run of fig_gui_rect calls: an
 *  arbitrary diagonal needs one or the other, and a 200-pixel diagonal is two
 *  triangles here against ~200 fill rectangles the other way. Same
 *  rdpq_triangle(&TRIFMT_SHADE, ...) shape a full-screen vignette effect
 *  would use inside the 2D pass.
 *
 *  Exists for fig_debugdraw, which projects world-space geometry into screen
 *  space and needs to join the results. It is a general primitive and a HUD is
 *  welcome to it, but note that a 1px diagonal on a 320x240 framebuffer is a
 *  stair-stepped, unantialiased line — fine for a debug overlay, not a
 *  substitute for authored art. */
void fig_gui_line(int x0, int y0, int x1, int y1, int thickness, color_t c);

#ifdef __cplusplus
}
#endif

#endif /* FIG_GUI_H */
