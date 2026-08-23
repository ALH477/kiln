/* SPDX-License-Identifier: MIT
 *
 * kiln_widget.c — see kiln_widget.h for the model.
 */

#include "kiln_widget.h"

#include <fmath.h>
#include <string.h>

KilnWidgetStyle kiln_widget_style_default(void)
{
    return (KilnWidgetStyle){
        .bg     = RGBA32( 10,  10,  24, 210),
        .border = RGBA32(139,  92, 246, 255),
        .text   = RGBA32(232, 232, 240, 255),
        .dim    = RGBA32(128, 128, 148, 255),
        .accent = RGBA32(  0, 245, 120, 255),
        .warn   = RGBA32(255, 120,  90, 255),
        /* No funk: straight rectangles that hold still. */
    };
}

KilnWidgetStyle kiln_widget_style_funky(void)
{
    KilnWidgetStyle st = kiln_widget_style_default();
    /* Numbers picked against a 320x240 framebuffer, where one pixel is a
     * lot. A 5 px lean across a 100 px panel is about 3 degrees — enough
     * that nothing lines up with the screen edge, little enough that text
     * inside it still reads. Past about 8 the panels start to look broken
     * rather than casual. */
    st.lean   = 5.0f;
    st.wobble = 1.6f;
    st.jitter = 2.2f;
    st.pop    = 3.0f;
    st.rate   = 0.42f;
    return st;
}

/* ── the clock ──────────────────────────────────────────────────────────*/

static float kiln_widget_clock;

void kiln_widget_tick(float dt)
{
    kiln_widget_clock += dt;
    /* Wrap on a whole number of seconds so the sines below stay continuous
     * across the wrap AND the float never grows large enough to lose
     * precision in its fractional part. 1024 s is about 17 minutes, longer
     * than any single screen will be up, and a power of two so the wrap is
     * exact. */
    if (kiln_widget_clock > 1024.0f) kiln_widget_clock -= 1024.0f;
}

float kiln_widget_time(void)
{
    return kiln_widget_clock;
}

float kiln_widget_jitter(uint32_t seed)
{
    /* Knuth's multiplicative hash, then the top bits mapped to [-1, 1].
     * Deterministic from the index alone: the same menu row is crooked the
     * same way every frame and in every session, which is the difference
     * between "hand-placed" and "static". */
    uint32_t h = seed * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return (float)(int32_t)(h >> 8) / 8388608.0f - 1.0f;
}

/** The shared sway. `phase` de-syncs one widget from another so a screenful
 *  of panels drifts as a crowd rather than as one rigid sheet. */
static float sway(const KilnWidgetStyle *st, float phase)
{
    if (st->wobble <= 0.0f) return 0.0f;
    return st->wobble * fm_sinf(kiln_widget_clock * st->rate * 6.2831853f
                                + phase);
}

void kiln_widget_panel_skew(int x, int y, int w, int h, float lean,
                           color_t fill, color_t border)
{
    if (h <= 0 || w <= 0) return;
    if (lean > -0.5f && lean < 0.5f) {
        kiln_gui_panel(x, y, w, h, fill, border);
        return;
    }

    /* kiln_gui has axis-aligned rects and nothing else, so a parallelogram is
     * a stack of bands, each nudged along. BAND is 2 px rather than 1 for
     * two reasons, and only one of them is cost: at 320x240 a 2 px step is
     * already below what reads as a stair, and halving the rect count halves
     * the RDP fill setup, which is the part of a fill rect that actually
     * costs anything. The other reason is that the slight chunkiness is on
     * theme — this is a game about goblins, not a spreadsheet.
     */
    const int BAND = 2;
    for (int row = 0; row < h; row += BAND) {
        int bh = (row + BAND > h) ? (h - row) : BAND;
        /* Bottom edge at offset 0, top edge at `lean`. */
        float t = 1.0f - (float)row / (float)h;
        int dx = (int)(lean * t);
        int edge_top = (row == 0);
        int edge_bot = (row + bh >= h);
        kiln_gui_rect(x + dx, y + row, w, bh, border);
        int inset_y = y + row + (edge_top ? 1 : 0);
        int inset_h = bh - (edge_top ? 1 : 0) - (edge_bot ? 1 : 0);
        if (inset_h > 0 && w > 2) {
            kiln_gui_rect(x + dx + 1, inset_y, w - 2, inset_h, fill);
        }
    }
}

/** Horizontal offset of a point `dy` px below a skewed panel's top edge. */
static int lean_at(float lean, int h, int dy)
{
    if (h <= 0) return 0;
    float t = 1.0f - (float)dy / (float)h;
    return (int)(lean * t);
}

static color_t with_alpha(color_t c, float f)
{
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return RGBA32(c.r, c.g, c.b, (uint8_t)(c.a * f));
}

/* ── Menu ──────────────────────────────────────────────────────────────*/

void kiln_menu_init(KilnMenu *m, int count, int visible_rows)
{
    m->cursor = 0;
    m->count = count < 0 ? 0 : count;
    m->scroll = 0;
    m->visible_rows = (uint8_t)(visible_rows < 0 ? 0 : visible_rows);
    m->wrap = 1;
}

/** Pull `scroll` toward the cursor so the cursor is always drawn. */
static void menu_reveal(KilnMenu *m)
{
    int vis = m->visible_rows ? m->visible_rows : m->count;
    if (vis <= 0 || vis >= m->count) { m->scroll = 0; return; }
    if (m->cursor < m->scroll) m->scroll = m->cursor;
    if (m->cursor >= m->scroll + vis) m->scroll = m->cursor - vis + 1;
    if (m->scroll > m->count - vis) m->scroll = m->count - vis;
    if (m->scroll < 0) m->scroll = 0;
}

void kiln_menu_set_count(KilnMenu *m, int count)
{
    m->count = count < 0 ? 0 : count;
    if (m->cursor >= m->count) m->cursor = m->count > 0 ? m->count - 1 : 0;
    menu_reveal(m);
}

int kiln_menu_move(KilnMenu *m, int delta)
{
    if (m->count <= 0) return 0;
    int c = m->cursor + delta;
    if (m->wrap) {
        /* Modulo, but correct for negatives — C's % gives -1 for (-1 % 4). */
        c %= m->count;
        if (c < 0) c += m->count;
    } else {
        if (c < 0) c = 0;
        if (c >= m->count) c = m->count - 1;
    }
    m->cursor = c;
    menu_reveal(m);
    return m->cursor;
}

int kiln_menu_move_enabled(KilnMenu *m, int delta, const uint8_t *enabled,
                          int count)
{
    if (!enabled || m->count <= 0) return kiln_menu_move(m, delta);
    if (count > m->count) count = m->count;

    int step = delta >= 0 ? 1 : -1;
    int remaining = delta >= 0 ? delta : -delta;
    int c = m->cursor;

    while (remaining > 0) {
        /* Walk one enabled row per unit of delta. The inner bound is
         * m->count so a list that is entirely disabled terminates instead
         * of spinning — in that case the cursor stays where it was. */
        int tries = 0;
        int next = c;
        do {
            next += step;
            if (m->wrap) {
                if (next < 0) next = m->count - 1;
                if (next >= m->count) next = 0;
            } else {
                if (next < 0 || next >= m->count) { next = c; break; }
            }
            tries++;
        } while (tries <= m->count &&
                 !(next < count ? enabled[next] : 0));

        if (next == c) break;
        if (!(next < count ? enabled[next] : 0)) break;
        c = next;
        remaining--;
    }

    m->cursor = c;
    menu_reveal(m);
    return m->cursor;
}

int kiln_menu_draw(const KilnMenu *m, int x, int y, int w,
                  const char *const *labels, const uint8_t *enabled,
                  const KilnWidgetStyle *st)
{
    int vis = m->visible_rows ? m->visible_rows : m->count;
    if (vis > m->count) vis = m->count;
    /* Rows need more vertical room once they are allowed to shift about. */
    int pitch = KILN_WIDGET_LINE_H + (st->jitter > 0.0f ? 3 : 0);
    int h = vis * pitch + 8;

    int panel_sway = (int)sway(st, 0.0f);
    x += panel_sway;
    kiln_widget_panel_skew(x, y, w, h, st->lean, st->bg, st->border);

    for (int i = 0; i < vis; i++) {
        int row = m->scroll + i;
        if (row >= m->count) break;
        const char *label = labels ? labels[row] : NULL;
        if (!label) continue;   /* spacer */

        int ry = y + 4 + i * pitch;
        int on = (enabled ? enabled[row] : 1);
        int sel = (row == m->cursor);

        /* Every row gets its own crookedness, hashed from its index so it
         * is the same crookedness every frame, plus a slow sway phased off
         * that same hash so the rows drift out of step with each other.
         * Rows that swayed together would just be a moving list. */
        int jx = (int)(st->jitter * kiln_widget_jitter(row * 2u + 1u));
        int jy = (int)(st->jitter * 0.45f * kiln_widget_jitter(row * 2u + 7u));
        float phase = kiln_widget_jitter(row * 2u + 3u) * 3.1416f;
        int rx = x + lean_at(st->lean, h, ry - y) + jx
               + (int)(sway(st, phase) * 0.6f);
        ry += jy;

        /* The selected row leans back the other way and sits proud of the
         * rest. A highlight bar alone tells you which row is selected; a
         * row that has visibly moved tells you the menu noticed you. */
        if (sel) {
            rx += (int)st->pop;
            kiln_gui_rect(rx + 1, ry - 1, w - 4, KILN_WIDGET_LINE_H + 2,
                         RGBA32(st->accent.r, st->accent.g, st->accent.b, 56));
            kiln_gui_rect(rx + 1, ry - 1, 2, KILN_WIDGET_LINE_H + 2, st->accent);
        }
        color_t c = !on ? st->dim : (sel ? st->accent : st->text);
        kiln_gui_text(rx + 6 + KILN_WIDGET_CHAR_W, ry + KILN_WIDGET_LINE_H - 3,
                     c, "%s", label);
        if (sel) {
            kiln_gui_text(rx + 5, ry + KILN_WIDGET_LINE_H - 3, st->accent, ">");
        }
    }

    /* Scroll indicator: a thin track on the right edge, only when the list
     * is actually longer than the window. */
    if (m->visible_rows && m->count > vis) {
        int track_h = h - 8;
        int knob_h = track_h * vis / m->count;
        if (knob_h < 4) knob_h = 4;
        int knob_y = y + 4 + (track_h - knob_h) * m->scroll /
                     (m->count - vis);
        kiln_gui_rect(x + w - 5 + lean_at(st->lean, h, 4), y + 4, 2,
                     track_h, st->dim);
        kiln_gui_rect(x + w - 6 + lean_at(st->lean, h, knob_y - y), knob_y, 4,
                     knob_h, st->accent);
    }

    return h;
}

/* ── Button ────────────────────────────────────────────────────────────*/

int kiln_widget_button(int x, int y, int w, int h, const char *label,
                      int selected, int pressed, const KilnWidgetStyle *st)
{
    color_t border = selected ? st->accent : st->border;
    color_t fill   = selected
        ? RGBA32(st->accent.r / 6, st->accent.g / 6, st->accent.b / 6, 230)
        : st->bg;
    /* A selected button leans the OPPOSITE way to an idle one — the snap
     * between the two as the cursor moves is most of the feedback. */
    float lean = selected ? -st->lean * 1.4f : st->lean;
    x += (int)sway(st, (float)(x + y) * 0.13f);
    if (selected) y -= (int)(st->pop * 0.5f);
    kiln_widget_panel_skew(x, y, w, h, lean, fill, border);

    if (label) {
        int len = (int)strlen(label);
        int tx = x + (w - len * KILN_WIDGET_CHAR_W) / 2 + lean_at(lean, h, h / 2);
        if (tx < x + 2) tx = x + 2;
        int ty = y + h / 2 + 4;
        kiln_gui_text(tx, ty, selected ? st->accent : st->text, "%s", label);
    }

    return selected && pressed;
}

/* ── Dice ──────────────────────────────────────────────────────────────*/

/* Pip layout: a 3x3 grid, cells numbered
 *     0 1 2
 *     3 4 5
 *     6 7 8
 * and one bitmask per face naming the cells that get a pip. This is the
 * standard western die arrangement; faces beyond 6 have no pip convention
 * and are drawn as a number instead. */
static const uint16_t k_pips[7] = {
    0,           /* unused: face 0 */
    1 << 4,                                        /* 1 */
    (1 << 0) | (1 << 8),                           /* 2 */
    (1 << 0) | (1 << 4) | (1 << 8),                /* 3 */
    (1 << 0) | (1 << 2) | (1 << 6) | (1 << 8),     /* 4 */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 8),          /* 5 */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 8) /* 6 */
};

void kiln_widget_dice(int x, int y, int size, int face, int rolling,
                     float anim_t, const KilnWidgetStyle *st)
{
    if (size < 12) size = 12;

    int shown = face;
    if (rolling) {
        /* Cycle at ~12 faces/second. Deterministic from anim_t, so the
         * spin does not depend on frame rate and a paused game holds a
         * still face rather than flickering. */
        int tick = (int)(anim_t * 12.0f);
        if (tick < 0) tick = -tick;
        shown = 1 + (tick % 6);
    }

    color_t body = rolling ? st->dim : st->text;
    kiln_gui_panel(x, y, size, size, st->bg, body);

    if (shown >= 1 && shown <= 6) {
        int pip = size / 7;
        if (pip < 2) pip = 2;
        int step = (size - pip) / 4;       /* 3 columns at 1/4, 2/4, 3/4 */
        uint16_t mask = k_pips[shown];
        for (int i = 0; i < 9; i++) {
            if (!(mask & (1u << i))) continue;
            int cx = x + step * (1 + (i % 3)) - pip / 2;
            int cy = y + step * (1 + (i / 3)) - pip / 2;
            kiln_gui_rect(cx, cy, pip, pip, rolling ? st->dim : st->accent);
        }
    } else if (shown > 0) {
        /* kiln_dice allows up to 16 faces; past 6 there is no pip pattern,
         * so show the number. */
        kiln_gui_text(x + size / 2 - KILN_WIDGET_CHAR_W / 2,
                     y + size / 2 + 4, st->accent, "%d", shown);
    }
}

/* ── Per-player HUD strip ──────────────────────────────────────────────*/

#define SLOT_H 20

int kiln_widget_hud_strip(int x, int y, int w, const KilnPlayerSlot *slots,
                         int count, const KilnWidgetStyle *st)
{
    int h = count * SLOT_H + 8;
    /* A third of the menus' lean. This one is read while the player is
     * trying to do something else, and a score column that leans as hard as
     * a title screen stops being legible and starts being in the way. */
    kiln_widget_panel_skew(x, y, w, h, st->lean * 0.34f, st->bg, st->border);

    for (int i = 0; i < count; i++) {
        const KilnPlayerSlot *s = &slots[i];
        int ry = y + 4 + i * SLOT_H;
        int lx = lean_at(st->lean * 0.34f, h, ry - y);
        /* Only the ACTIVE player's row sways — it is a cheap way to draw
         * the eye to whose turn it is without another colour. */
        if (s->active) lx += (int)sway(st, (float)i);
        x += lx;

        if (s->active) {
            kiln_gui_rect(x + 2, ry, w - 4, SLOT_H,
                         RGBA32(st->accent.r, st->accent.g, st->accent.b, 40));
        }
        /* Colour swatch: the only thing on the strip that identifies a
         * player at a glance from across a room, so it goes first and is
         * the full row height. */
        kiln_gui_rect(x + 4, ry + 2, 5, SLOT_H - 4, s->tint);

        color_t name_c = s->active ? st->accent : st->text;
        kiln_gui_text(x + 13, ry + 9, name_c, "%-8s", s->name ? s->name : "?");
        kiln_gui_text(x + 13 + 8 * KILN_WIDGET_CHAR_W, ry + 9, st->text,
                     "%4d", (int)s->score);

        /* A status note takes the charge bar's slot rather than a line of
         * its own. It used to be drawn under the name, where at SLOT_H of 20
         * it landed on the next player's row; and when a goblin is couch-
         * locked, that he is couch-locked matters more at a glance than how
         * close his special is. */
        if (s->note) {
            kiln_gui_text(x + 15 + 13 * KILN_WIDGET_CHAR_W, ry + 13,
                         st->warn, "%s", s->note);
        } else if (s->charge >= 0.0f) {
            int bx = x + 13 + 13 * KILN_WIDGET_CHAR_W;
            int bw = w - (bx - x) - 8;
            if (bw > 8) {
                kiln_gui_bar(bx, ry + 6, bw, 5, s->charge,
                            s->ready ? st->accent : st->border,
                            RGBA32(30, 30, 48, 255));
            }
        }
        x -= lx;
    }

    return h;
}

/* ── Results panel ─────────────────────────────────────────────────────*/

int kiln_widget_results(int x, int y, int w, const char *title,
                       const KilnPlayerSlot *slots, const int *order,
                       int count, const KilnWidgetStyle *st)
{
    int h = count * KILN_WIDGET_LINE_H + 12 + KILN_WIDGET_LINE_H + 6;
    kiln_widget_panel_skew(x, y, w, h, st->lean, st->bg, st->border);

    if (title) {
        int len = (int)strlen(title);
        int tx = x + (w - len * KILN_WIDGET_CHAR_W) / 2
               + lean_at(st->lean, h, KILN_WIDGET_LINE_H);
        if (tx < x + 4) tx = x + 4;
        kiln_gui_text(tx, y + KILN_WIDGET_LINE_H, st->accent, "%s", title);
    }
    kiln_gui_rect(x + 4 + lean_at(st->lean, h, KILN_WIDGET_LINE_H + 4),
                 y + KILN_WIDGET_LINE_H + 4, w - 8, 1, st->border);

    for (int i = 0; i < count; i++) {
        int idx = order ? order[i] : i;
        if (idx < 0) continue;
        const KilnPlayerSlot *s = &slots[idx];
        int ry = y + KILN_WIDGET_LINE_H + 16 + i * KILN_WIDGET_LINE_H;

        /* First place gets the accent; everyone else is plain. Ties are the
         * caller's problem — it built `order`. */
        color_t c = (i == 0) ? st->accent : st->text;
        /* The winner's row sways; the rest sit still. Placing is the whole
         * point of this panel, so the first row is the one that moves. */
        int lx = lean_at(st->lean, h, ry - y)
               + (i == 0 ? (int)(sway(st, 0.0f) * 1.4f) : 0);
        kiln_gui_rect(x + lx + 6, ry - 7, 5, 8, s->tint);
        kiln_gui_text(x + lx + 14, ry, c, "%d.", i + 1);
        kiln_gui_text(x + lx + 14 + 3 * KILN_WIDGET_CHAR_W, ry, c,
                     "%-8s", s->name ? s->name : "?");
        kiln_gui_text(x + lx + 14 + 12 * KILN_WIDGET_CHAR_W, ry, c,
                     "%4d", (int)s->score);
    }

    return h;
}

/* ── Banner ────────────────────────────────────────────────────────────*/

void kiln_widget_banner(int x, int y, int w, int h, const char *text,
                       float fade, const KilnWidgetStyle *st)
{
    /* A banner leans harder than anything else on screen and sways on its
     * own phase. It is the one widget whose whole job is to be noticed. */
    float lean = -st->lean * 1.8f;
    int sx = x + (int)(sway(st, 1.7f) * 1.5f);
    int sy = y + (int)(sway(st, 0.4f) * 0.5f);
    kiln_widget_panel_skew(sx, sy, w, h, lean,
                          with_alpha(st->bg, fade),
                          with_alpha(st->accent, fade));
    if (text) {
        int len = (int)strlen(text);
        int tx = sx + (w - len * KILN_WIDGET_CHAR_W) / 2
               + lean_at(lean, h, h / 2);
        if (tx < sx + 2) tx = sx + 2;
        kiln_gui_text(tx, sy + h / 2 + 4, with_alpha(st->accent, fade),
                     "%s", text);
    }
}
