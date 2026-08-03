/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_gui.h — the 2D half of the M64 engine.
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
 * m64_gui_begin() performs the one RDP state transition from the 3D pass:
 * depth test off, standard combiner, alpha blending on. Every widget below
 * assumes that has happened. Calling them inside the 3D pass will produce
 * depth-tested, possibly occluded 2D — which is exactly the bug the explicit
 * bracket exists to make obvious.
 */
#ifndef M64_GUI_H
#define M64_GUI_H

#include <libdragon.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Font slot used for the built-in monospace debug font. Registered by
 *  m64_gui_init(); needs no font asset in the filesystem. */
#define M64_GUI_FONT 1

/** Called by m64_engine_init(). Registers the built-in font. */
void m64_gui_init(void);
void m64_gui_close(void);

/** Bracket the 2D pass. Must be called after the 3D pass, inside the same
 *  m64_frame_begin/m64_frame_end. */
void m64_gui_begin(void);
void m64_gui_end(void);

/** Solid filled rectangle. */
void m64_gui_rect(int x, int y, int w, int h, color_t c);

/** Panel: filled body plus a 1px border. The workhorse for HUD boxes. */
void m64_gui_panel(int x, int y, int w, int h, color_t fill, color_t border);

/** Text at a baseline position, printf-style. */
void m64_gui_text(int x, int y, color_t c, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/** Horizontal progress/meter bar. `frac` is clamped to [0,1]. */
void m64_gui_bar(int x, int y, int w, int h, float frac, color_t fg, color_t bg);

#ifdef __cplusplus
}
#endif

#endif /* M64_GUI_H */
