/* SPDX-License-Identifier: MIT
 *
 * kiln_dialogue.h — NPC dialogue text boxes. A multi-page text box system
 * built on fig_gui_panel + fig_gui_text, with a typewriter reveal effect.
 *
 * ── OoT + Half-Life NPC talk ────────────────────────────────────────────
 * Both games present NPC dialogue in a bottom-of-screen text box. OoT
 * uses a portrait + multi-page text with A to advance. HL uses a
 * single-line subtitle at the bottom. This module supports both: the
 * text box is configurable in size, and multi-page text is supported.
 *
 * ── Typewriter effect ──────────────────────────────────────────────────
 * Characters are revealed one at a time at ~30 chars/sec. Press A to
 * skip to the full line. Press A again to advance to the next page.
 * After the last page, the dialogue closes. While dialogue is active,
 * the game should pause player movement (the module does not enforce
 * this — the game checks fig_dialogue_active).
 *
 * ── No text wrapping ────────────────────────────────────────────────────
 * fig_gui_text is single-line. Lines longer than the text box width are
 * simply clipped by the RDP. The game is responsible for keeping lines
 * short enough to fit (~36 chars at 320×240 with the debug font). This
 * is a deliberate limit, not a bug — a full text layout engine is not
 * worth the code on a console where every line is hand-authored.
 */
#ifndef FIG_DIALOGUE_H
#define FIG_DIALOGUE_H


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

#include <stdint.h>
#include "kiln_input.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FIG_DIALOGUE_MAX_LINES 8
#define FIG_DIALOGUE_MAX_PAGES 2

typedef struct {
    const char *lines[FIG_DIALOGUE_MAX_LINES];
    uint8_t line_count;
    uint8_t current_line;
    uint8_t char_count;
    float   char_timer;
    uint8_t active;
    int     box_y;
    int     box_h;
} FigDialogue;

/** Start a dialogue session with the given lines. `count` is the number
 *  of lines (max FIG_DIALOGUE_MAX_LINES). The dialogue is active
 *  immediately. */
void fig_dialogue_start(FigDialogue *d, const char *lines[], int count);

/** Advance the dialogue one frame. Handles typewriter reveal and
 *  A-button input (skip/advance/close). Returns 1 if still active,
 *  0 if finished. */
int fig_dialogue_update(FigDialogue *d, float dt, const FigInput *in);

/** Draw the dialogue text box in the 2D pass. Draws a panel at the
 *  bottom of the screen and the current line text with typewriter
 *  reveal. */
void fig_dialogue_draw(FigDialogue *d);

/** Returns 1 if dialogue is currently active (player should be paused). */
int fig_dialogue_active(const FigDialogue *d);

#ifdef __cplusplus
}
#endif

#endif /* FIG_DIALOGUE_H */