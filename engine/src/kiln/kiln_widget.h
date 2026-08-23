/* SPDX-License-Identifier: MIT
 *
 * kiln_widget.h — composite screen furniture layered on kiln_gui.
 *
 * kiln_gui gives you rect/panel/text/bar. That is the right primitive set
 * for a HUD, and the wrong granularity for a game that also has a title
 * screen, a character-select grid, a dice roll, a per-player status strip
 * and a results table — each of which is a dozen gui calls in a fixed
 * arrangement that every screen would otherwise re-derive slightly
 * differently. This header is those arrangements, once.
 *
 * ── Still immediate mode ───────────────────────────────────────────────
 * Nothing here retains anything between frames, and there is no widget
 * tree, no parent pointers, no invalidation. kiln_gui.h's rationale for
 * that (a 93.75 MHz VR4300 should not be walking a tree to draw twelve
 * rectangles) applies with exactly the same force one layer up.
 *
 * The one place state is unavoidable is a menu cursor — "which row is
 * selected" has to survive to the next frame or the player cannot move it.
 * That state is CALLER-OWNED: KilnMenu is a plain struct the caller keeps
 * (in its scene state, on the stack, wherever), passed in on every draw.
 * The widget layer never allocates one and never owns one. This is the
 * standard immediate-mode answer and it is not a retained tree: there is
 * no hierarchy, no lifetime to manage, and a caller that stops drawing a
 * menu simply stops passing it.
 *
 * ── Input is passed in, not polled ─────────────────────────────────────
 * kiln_widget_button takes `selected` and `pressed` rather than reading
 * kiln_input itself. The engine already has exactly one poll-per-frame
 * point (kiln_input_update); a widget that polled again would be a second
 * source of truth for the same buttons, and a widget that polled a fixed
 * port could not be driven by a menu that four players share. Screens
 * read input once and tell the widgets what happened.
 *
 * ── Layout units ───────────────────────────────────────────────────────
 * Positions are screen pixels, top-left origin, same as kiln_gui.
 * KILN_WIDGET_CHAR_W / LINE_H are the built-in debug mono font's advance
 * and line pitch; they are what the layout maths uses to size text against
 * its panel. Swap the font via rdpq and these become wrong — they are
 * approximations for one specific built-in font, not a metrics API.
 */
#ifndef KILN_WIDGET_H
#define KILN_WIDGET_H

#include <libdragon.h>

#include "kiln_gui.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Advance width and line pitch of the built-in debug mono font, in pixels. */
#define KILN_WIDGET_CHAR_W 8
#define KILN_WIDGET_LINE_H 12

/** Colour set shared by every widget below. One struct rather than a
 *  colour argument per call, because a screen wants all its furniture to
 *  agree and threading five colours through eight calls does not scale.
 *  kiln_widget_style_default() fills in the engine's house palette. */
typedef struct {
    color_t bg;       /**< panel fill                                   */
    color_t border;   /**< panel border                                 */
    color_t text;     /**< normal text                                  */
    color_t dim;      /**< de-emphasised text (unselected, disabled)    */
    color_t accent;   /**< selection highlight, active player, "ready"  */
    color_t warn;     /**< negative deltas, hazards, cooldowns          */

    /* ── the funk ──────────────────────────────────────────────────────
     * Everything below is zero in the default style, and with all of it at
     * zero every widget here draws exactly the axis-aligned rectangles it
     * always did. That default matters: a HUD that has to be read at a
     * glance during play wants to sit still, and a debug overlay wants to
     * be boring. The funk is for the screens where the game is showing off.
     *
     * The parameters are deliberately geometric rather than a "theme" enum,
     * because the look wanted here — hand-placed, slightly wrong, never
     * quite still — is not one preset but a set of small independent
     * deviations from the grid.
     */
    float lean;     /**< px a panel's top edge sits right of its bottom.
                      *  Turns every panel into a parallelogram. The single
                      *  biggest contributor to "off-kilter": a rectangle
                      *  reads as a UI, a parallelogram reads as a sticker
                      *  someone slapped on.                              */
    float wobble;   /**< px amplitude of the idle sway. Everything drifts
                      *  on a slow sine; nothing on screen is ever exactly
                      *  where it was a second ago.                       */
    float jitter;   /**< px of per-item deterministic offset and tilt.
                      *  Hashed from the item's index, NOT random — a menu
                      *  that reshuffles its own crookedness every frame is
                      *  noise, and one that is crooked the same way every
                      *  time reads as hand-placed.                       */
    float pop;      /**< px a selected row grows by, with an overshoot on
                      *  the way in.                                      */
    float rate;     /**< wobble cycles per second.                        */
} KilnWidgetStyle;

/** The engine's default palette: dark violet panels, green accent, and no
 *  funk at all — straight rectangles that hold still. */
KilnWidgetStyle kiln_widget_style_default(void);

/** The party-game preset: leaning panels, a slow sway, per-item crookedness
 *  and a selection that overshoots. Same palette as the default. */
KilnWidgetStyle kiln_widget_style_funky(void);

/** Advance the widget layer's animation clock. Call once per frame, before
 *  drawing, whatever screen is up.
 *
 *  A module-global clock rather than a per-widget one because every widget
 *  on screen must sway in the same time base — two panels breathing at
 *  independent phases reads as a bug, not as life. Immediate-mode widgets
 *  have nowhere else to keep it: they are called fresh every frame and
 *  retain nothing between calls, which is exactly the property that makes
 *  the ONE piece of genuinely temporal state need a home here. */
void kiln_widget_tick(float dt);

/** The clock's current value in seconds. Exposed so a game can phase its own
 *  flourishes against the same beat the widgets are using. */
float kiln_widget_time(void);

/** A parallelogram: `lean` px of horizontal offset from bottom edge to top.
 *  Drawn as a stack of short bands, because kiln_gui has only axis-aligned
 *  rects — see the .c for why the band height is a feature and not a
 *  limitation. Pass lean 0 for a plain panel. */
void kiln_widget_panel_skew(int x, int y, int w, int h, float lean,
                           color_t fill, color_t border);

/** Deterministic per-item offset in [-1, 1], hashed from `seed`. The same
 *  seed always gives the same number, which is what makes jitter read as
 *  placement rather than as noise. Exposed because a game laying out its own
 *  screens wants its decorations crooked in the same style. */
float kiln_widget_jitter(uint32_t seed);

/* ── Menu ──────────────────────────────────────────────────────────────
 * A vertical list with a cursor and optional scrolling. Caller-owned
 * state; see the file comment for why this is not a retained widget.
 */
typedef struct {
    int     cursor;        /**< selected row, always in [0, count)       */
    int     count;         /**< number of rows                           */
    int     scroll;        /**< index of the first visible row           */
    uint8_t visible_rows;  /**< rows drawn at once; 0 means "all"        */
    uint8_t wrap;          /**< 1: moving off an end wraps to the other  */
} KilnMenu;

/** Reset a menu to row 0. `visible_rows` of 0 draws every row (no scroll). */
void kiln_menu_init(KilnMenu *m, int count, int visible_rows);

/** Change the row count, clamping the cursor and scroll to stay valid.
 *  Use when a list's contents change (e.g. a board list filtered by mode). */
void kiln_menu_set_count(KilnMenu *m, int count);

/** Move the cursor by `delta` rows, honouring `wrap`, and scroll the
 *  visible window to keep the cursor on screen. Returns the new cursor. */
int kiln_menu_move(KilnMenu *m, int delta);

/** Draw the menu's rows inside a panel at (x,y) of width `w`. `labels` is
 *  an array of `m->count` strings; a NULL entry draws as a blank spacer
 *  row (useful for separating groups). `enabled`, if non-NULL, is an array
 *  of `m->count` flags — a 0 entry draws dim and is skipped by
 *  kiln_menu_move. Returns the panel's total height in pixels so the caller
 *  can lay something out beneath it. */
int kiln_menu_draw(const KilnMenu *m, int x, int y, int w,
                  const char *const *labels, const uint8_t *enabled,
                  const KilnWidgetStyle *st);

/** Like kiln_menu_move but skips rows whose `enabled` entry is 0. Pass the
 *  same array given to kiln_menu_draw. Returns the new cursor; if every row
 *  is disabled the cursor does not move. */
int kiln_menu_move_enabled(KilnMenu *m, int delta, const uint8_t *enabled,
                          int count);

/* ── Button ────────────────────────────────────────────────────────────*/

/** A labelled box. `selected` draws the accent border + accent text;
 *  `pressed` is the caller's edge-triggered confirm for this frame.
 *  Returns 1 exactly when the button is both selected and pressed — i.e.
 *  "this button was activated" — so call sites read as
 *  `if (kiln_widget_button(...)) do_the_thing();`. */
int kiln_widget_button(int x, int y, int w, int h, const char *label,
                      int selected, int pressed, const KilnWidgetStyle *st);

/* ── Dice ──────────────────────────────────────────────────────────────*/

/** A die face drawn as pips on a rounded-ish square of side `size`.
 *  `face` is 1-6; anything else draws blank. When `rolling` is non-zero the
 *  pips are shuffled from `anim_t` (seconds since the roll started) rather
 *  than showing `face`, so the caller can run a spin-up before revealing
 *  the real result — pass the true face throughout and flip `rolling` to 0
 *  at the reveal. Faces above 6 (kiln_dice supports up to 16) draw the
 *  number instead of pips. */
void kiln_widget_dice(int x, int y, int size, int face, int rolling,
                     float anim_t, const KilnWidgetStyle *st);

/* ── Per-player HUD strip ──────────────────────────────────────────────*/

/** One player's line in the HUD strip / results table. `name` and `note`
 *  are borrowed for the duration of the call only. */
typedef struct {
    const char *name;    /**< character or player name                  */
    const char *note;    /**< short status text, or NULL ("COUCH", ...) */
    int32_t     score;   /**< buds, points, whatever the game counts    */
    float       charge;  /**< special charge in [0,1]; <0 draws no bar  */
    color_t     tint;    /**< player colour, drawn as a swatch          */
    uint8_t     active;  /**< 1 draws the accent highlight              */
    uint8_t     ready;   /**< 1 draws the charge bar in `accent`        */
} KilnPlayerSlot;

/** A compact one-row-per-player strip: colour swatch, name, score, and a
 *  charge bar. Sized to fit `count` rows; returns its total height. */
int kiln_widget_hud_strip(int x, int y, int w, const KilnPlayerSlot *slots,
                         int count, const KilnWidgetStyle *st);

/* ── Results panel ─────────────────────────────────────────────────────*/

/** A ranked end-of-match table. `order` is an array of `count` slot
 *  indices, first place first — the caller sorts, because only the game
 *  knows its tiebreak rules. `title` is drawn as a header. Returns the
 *  panel's total height. */
int kiln_widget_results(int x, int y, int w, const char *title,
                       const KilnPlayerSlot *slots, const int *order,
                       int count, const KilnWidgetStyle *st);

/* ── Banner ────────────────────────────────────────────────────────────*/

/** A centred single-line callout across width `w` — "ROUND 3", "HARVEST
 *  EVENT!", "DANK WINS". `fade` in [0,1] scales the alpha of everything
 *  drawn, so a caller can run its own in/out envelope without a second
 *  entry point. */
void kiln_widget_banner(int x, int y, int w, int h, const char *text,
                       float fade, const KilnWidgetStyle *st);

#ifdef __cplusplus
}
#endif

#endif /* KILN_WIDGET_H */
