/* SPDX-License-Identifier: MIT
 *
 * kiln_dialogue.h — NPC dialogue text boxes. A multi-page text box system
 * built on kiln_gui_panel + kiln_gui_text, with a typewriter reveal effect.
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
 * this — the game checks kiln_dialogue_active).
 *
 * ── No text wrapping ────────────────────────────────────────────────────
 * kiln_gui_text is single-line. Lines longer than the text box width are
 * simply clipped by the RDP. The game is responsible for keeping lines
 * short enough to fit (~36 chars at 320×240 with the debug font). This
 * is a deliberate limit, not a bug — a full text layout engine is not
 * worth the code on a console where every line is hand-authored.
 */
#ifndef KILN_DIALOGUE_H
#define KILN_DIALOGUE_H

#include <stdint.h>
#include "kiln_input.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KILN_DIALOGUE_MAX_LINES 8
#define KILN_DIALOGUE_MAX_PAGES 2

typedef struct {
    const char *lines[KILN_DIALOGUE_MAX_LINES];
    uint8_t line_count;
    uint8_t current_line;
    uint8_t char_count;
    float   char_timer;
    uint8_t active;
    int     box_y;
    int     box_h;
} KilnDialogue;

/** Start a dialogue session with the given lines. `count` is the number
 *  of lines (max KILN_DIALOGUE_MAX_LINES). The dialogue is active
 *  immediately. */
void kiln_dialogue_start(KilnDialogue *d, const char *lines[], int count);

/** Advance the dialogue one frame. Handles typewriter reveal and
 *  A-button input (skip/advance/close). Returns 1 if still active,
 *  0 if finished. */
int kiln_dialogue_update(KilnDialogue *d, float dt, const KilnInput *in);

/** Draw the dialogue text box in the 2D pass. Draws a panel at the
 *  bottom of the screen and the current line text with typewriter
 *  reveal. */
void kiln_dialogue_draw(KilnDialogue *d);

/** Returns 1 if dialogue is currently active (player should be paused). */
int kiln_dialogue_active(const KilnDialogue *d);

#ifdef __cplusplus
}
#endif

#endif /* KILN_DIALOGUE_H */