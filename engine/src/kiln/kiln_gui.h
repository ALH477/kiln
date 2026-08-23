/* SPDX-License-Identifier: MIT
 *
 * kiln_gui.h — the 2D half of the Kiln engine.
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
 * kiln_gui_begin() performs the one RDP state transition from the 3D pass:
 * depth test off, standard combiner, alpha blending on. Every widget below
 * assumes that has happened. Calling them inside the 3D pass will produce
 * depth-tested, possibly occluded 2D — which is exactly the bug the explicit
 * bracket exists to make obvious.
 */
#ifndef KILN_GUI_H
#define KILN_GUI_H

#include <libdragon.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Font slot used for the built-in monospace debug font. Registered by
 *  kiln_gui_init(); needs no font asset in the filesystem.
 *
 *  kiln_engine_init() ALREADY CALLS THIS. Calling it again is safe (the
 *  function is idempotent) but unnecessary — a ROM only needs it if it
 *  brought up display/rdpq/t3d by hand instead of via kiln_engine_init. */
#define KILN_GUI_FONT 1

/** Called by kiln_engine_init(). Registers the built-in font. */
void kiln_gui_init(void);
void kiln_gui_close(void);

/** Bracket the 2D pass. Must be called after the 3D pass, inside the same
 *  kiln_frame_begin/kiln_frame_end. */
void kiln_gui_begin(void);
void kiln_gui_end(void);

/** Solid filled rectangle. */
void kiln_gui_rect(int x, int y, int w, int h, color_t c);

/** Panel: filled body plus a 1px border. The workhorse for HUD boxes. */
void kiln_gui_panel(int x, int y, int w, int h, color_t fill, color_t border);

/** Text at a baseline position, printf-style. */
void kiln_gui_text(int x, int y, color_t c, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/** Horizontal progress/meter bar. `frac` is clamped to [0,1]. */
void kiln_gui_bar(int x, int y, int w, int h, float frac, color_t fg, color_t bg);

/** Straight line of `thickness` pixels between two screen points.
 *
 *  Drawn as two shaded triangles rather than a run of kiln_gui_rect calls: an
 *  arbitrary diagonal needs one or the other, and a 200-pixel diagonal is two
 *  triangles here against ~200 fill rectangles the other way. Same
 *  rdpq_triangle(&TRIFMT_SHADE, ...) shape a full-screen vignette effect
 *  would use inside the 2D pass.
 *
 *  Exists for kiln_debugdraw, which projects world-space geometry into screen
 *  space and needs to join the results. It is a general primitive and a HUD is
 *  welcome to it, but note that a 1px diagonal on a 320x240 framebuffer is a
 *  stair-stepped, unantialiased line — fine for a debug overlay, not a
 *  substitute for authored art. */
void kiln_gui_line(int x0, int y0, int x1, int y1, int thickness, color_t c);

#ifdef __cplusplus
}
#endif

#endif /* KILN_GUI_H */
