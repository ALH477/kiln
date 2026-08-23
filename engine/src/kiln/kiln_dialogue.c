/* SPDX-License-Identifier: MIT
 *
 * kiln_dialogue.c — see kiln_dialogue.h for the model.
 */

#include "kiln_dialogue.h"
#include "kiln_gui.h"

#include <libdragon.h>
#include <string.h>

#define CHAR_RATE 30.0f

void kiln_dialogue_start(KilnDialogue *d, const char *lines[], int count)
{
    memset(d->lines, 0, sizeof(d->lines));
    for (int i = 0; i < count && i < KILN_DIALOGUE_MAX_LINES; i++)
        d->lines[i] = lines[i];
    d->line_count = count;
    d->current_line = 0;
    d->char_count = 0;
    d->char_timer = 0.0f;
    d->active = 1;
    d->box_y = 160;
    d->box_h = 72;
}

int kiln_dialogue_update(KilnDialogue *d, float dt, const KilnInput *in)
{
    if (!d->active) return 0;

    const char *line = d->lines[d->current_line];
    int len = line ? (int)strlen(line) : 0;

    d->char_timer += dt;
    if (d->char_timer >= 1.0f / CHAR_RATE) {
        d->char_timer = 0.0f;
        if (d->char_count < len) d->char_count++;
    }

    if (in->edges & KILN_BTN_A) {
        if (d->char_count < len) {
            d->char_count = len;
        } else {
            d->current_line++;
            d->char_count = 0;
            d->char_timer = 0.0f;
            if (d->current_line >= d->line_count) {
                d->active = 0;
                return 0;
            }
        }
    }
    return 1;
}

void kiln_dialogue_draw(KilnDialogue *d)
{
    if (!d->active) return;
    kiln_gui_panel(8, d->box_y, 320 - 16, d->box_h,
                  RGBA32(10, 10, 24, 220), RGBA32(0, 245, 212, 255));
    const char *line = d->lines[d->current_line];
    if (line && d->char_count > 0) {
        char buf[192];
        int n = d->char_count;
        if (n > 191) n = 191;
        memcpy(buf, line, n);
        buf[n] = 0;
        kiln_gui_text(16, d->box_y + 12, RGBA32(232, 232, 240, 255), "%s", buf);
    }
    if (d->char_count >= (line ? (int)strlen(line) : 0)) {
        kiln_gui_text(320 - 60, d->box_y + d->box_h - 14,
                     RGBA32(0, 245, 212, 255), "A>");
    }
}

int kiln_dialogue_active(const KilnDialogue *d) { return d->active; }