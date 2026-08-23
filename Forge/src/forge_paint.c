/* SPDX-License-Identifier: MIT
 *
 * forge_paint.c — M4: the texture pack, edited on the console.
 *
 * Two halves, because "texture" means two different jobs:
 *
 *   PAINT  a 16x16 pixel editor over one atlas tile, 16 palette entries.
 *   BLOCK  which tile a block type uses — the assignment side.
 *
 * ── Why an in-ROM pixel editor is not a novelty ────────────────────────
 *
 * Because a palette-swap filter can discard hue (see the n64-modeling skill's
 * "CI4 and a palette-swap contract"): under one, only VALUE carries, so a
 * palette separated by hue stops reading entirely the moment the filter goes
 * up. That is a judgement about a 16-colour ramp seen at 320x240 through a
 * TLUT swap on a CRT, and there is no host preview that settles it — you look
 * at the swapped state on the television or you guess. `kiln_voxatlas_bind`
 * takes the state, so `Z` here flips between cold and veiled with the
 * geometry still on screen behind the editor.
 *
 * ── Why the edit buffer is one byte per pixel ──────────────────────────
 *
 * The atlas ships as CI4 (2 KB for 64x64 against a 4 KB TMEM), but painting a
 * nibble-packed buffer makes every plot a read-modify-write and every bug an
 * off-by-one nibble. kiln_voxmesh keeps a plain uint8_t index array and packs on
 * upload; 4 KB of RDRAM is a much better trade than that class of bug.
 */
#include "forge.h"

/* Sized and placed to CLEAR the shared status block (left column, y 8..60) and
 * the mode help line (y 230). The first version used CELL 8 at x 96, which put a
 * 128-pixel canvas across the middle of a 320x240 screen and straight over both —
 * the readouts drew on top of each other and were unreadable, which for an
 * overlay is the same as not being there. Half the size, hard right. */
#define SWATCH    7          /* palette swatch size, pixels                  */
#define CELL      5          /* on-screen size of one texel of the tile      */
#define CANVAS_X 228
#define CANVAS_Y  30

/* Draw one 16-entry TLUT entry as an on-screen colour. The atlas stores
 * RGBA5551 big-endian (the format rdpq_tex_upload_tlut wants); the GUI wants
 * RGBA8, so expand. Index 0 is transparent and is drawn as a checker so "empty"
 * is distinguishable from "black", which at 16 values is otherwise a guess. */
static color_t tlut_to_gui(uint16_t c)
{
    int r = (c >> 11) & 0x1F, g = (c >> 6) & 0x1F, b = (c >> 1) & 0x1F;
    return RGBA32((uint8_t)(r * 255 / 31), (uint8_t)(g * 255 / 31),
                  (uint8_t)(b * 255 / 31), 255);
}

void forge_paint_update(Forge *f, const KilnInput *in)
{
    /* D-pad moves the texel cursor. Held, not edged, because dragging a cursor
     * across a 16x16 grid one press at a time is 15 presses per row. */
    static int repeat;
    if (in->buttons & (KILN_BTN_DU | KILN_BTN_DD | KILN_BTN_DL | KILN_BTN_DR)) {
        if (repeat == 0 || repeat > 8) {
            if (in->buttons & KILN_BTN_DL) f->paint_x--;
            if (in->buttons & KILN_BTN_DR) f->paint_x++;
            if (in->buttons & KILN_BTN_DU) f->paint_y--;
            if (in->buttons & KILN_BTN_DD) f->paint_y++;
        }
        repeat++;
    } else {
        repeat = 0;
    }
    if (f->paint_x < 0) f->paint_x = KILN_VOXATLAS_TILE - 1;
    if (f->paint_y < 0) f->paint_y = KILN_VOXATLAS_TILE - 1;
    if (f->paint_x >= KILN_VOXATLAS_TILE) f->paint_x = 0;
    if (f->paint_y >= KILN_VOXATLAS_TILE) f->paint_y = 0;

    /* A plots, B picks. Held A paints a stroke — this is the one place a held
     * button SHOULD repeat, because drawing a line is the operation. */
    if (in->buttons & KILN_BTN_A)
        kiln_voxatlas_plot(&f->atlas, f->paint_tile, f->paint_x, f->paint_y,
                          f->paint_colour);
    if (in->edges & KILN_BTN_B)
        f->paint_colour = kiln_voxatlas_peek(&f->atlas, f->paint_tile,
                                            f->paint_x, f->paint_y);

    /* C-left/right cycle the colour, C-up/down the tile. The tile IS the block
     * type minus one, so cycling the tile also selects what GEO will place —
     * one control for one concept rather than two that can disagree. */
    if (in->edges & KILN_BTN_CR)
        f->paint_colour = (uint8_t)((f->paint_colour + 1) % KILN_VOXATLAS_COLOURS);
    if (in->edges & KILN_BTN_CL)
        f->paint_colour = (uint8_t)((f->paint_colour + KILN_VOXATLAS_COLOURS - 1)
                                    % KILN_VOXATLAS_COLOURS);
    if (in->edges & KILN_BTN_CU) {
        f->paint_tile = (f->paint_tile + 1) % KILN_VOXATLAS_TILES;
        f->block = (uint8_t)(f->paint_tile + 1);
    }
    if (in->edges & KILN_BTN_CD) {
        f->paint_tile = (f->paint_tile + KILN_VOXATLAS_TILES - 1) % KILN_VOXATLAS_TILES;
        f->block = (uint8_t)(f->paint_tile + 1);
    }

    /* Flood the whole tile, for starting from a base colour rather than from
     * the placeholder pattern. */
    if (in->edges & KILN_BTN_R)
        kiln_voxatlas_fill_tile(&f->atlas, f->paint_tile, f->paint_colour);

    /* Z previews the veiled palette. The whole reason this editor is on the
     * console: the veil collapses hue, so whether a ramp still separates is a
     * question about the television. */
    f->paint_veiled = (in->buttons & KILN_BTN_Z) ? 1 : 0;

    /* A block type's tile changed, so every chunk using it is stale — but only
     * its TEXTURE changed, not its geometry, and the atlas is uploaded per frame
     * from the same index array. So there is nothing to remesh: the UVs already
     * point at this tile. Noting it because "the texture changed, remesh" is the
     * reflex and it would be 24 chunks of pointless work per plotted pixel. */
}

void forge_paint_draw(Forge *f)
{
    const KilnVoxAtlasState st = f->paint_veiled ? KILN_VOXATLAS_VEILED
                                                : KILN_VOXATLAS_COLD;

    /* The tile, as CELL-sized rects. Not a blit of the real surface: that would
     * need the CI4 texture bound in the 2D pass at 8x magnification, and what is
     * wanted here is a grid with a cursor, not a preview — the preview is the
     * geometry behind this panel, drawn with this very atlas. */
    kiln_gui_panel(CANVAS_X - 2, CANVAS_Y - 2,
                  KILN_VOXATLAS_TILE * CELL + 4, KILN_VOXATLAS_TILE * CELL + 4,
                  RGBA32(0, 0, 0, 200), RGBA32(200, 200, 200, 255));

    for (int y = 0; y < KILN_VOXATLAS_TILE; y++)
        for (int x = 0; x < KILN_VOXATLAS_TILE; x++) {
            uint8_t idx = kiln_voxatlas_peek(&f->atlas, f->paint_tile, x, y);
            int px = CANVAS_X + x * CELL, py = CANVAS_Y + y * CELL;
            if (idx == 0) {
                /* Transparent: a checker, so "no colour" cannot be mistaken for
                 * "black". At 16 entries that confusion is one wasted slot. */
                color_t a = RGBA32(60, 60, 60, 255), b = RGBA32(90, 90, 90, 255);
                kiln_gui_rect(px, py, CELL, CELL, ((x ^ y) & 1) ? a : b);
            } else {
                kiln_gui_rect(px, py, CELL, CELL, tlut_to_gui(f->atlas.tlut[st][idx]));
            }
        }

    /* The cursor, as a ring rather than a fill, so the texel under it is still
     * visible — which is the whole question when picking a colour. */
    int cx = CANVAS_X + f->paint_x * CELL, cy = CANVAS_Y + f->paint_y * CELL;
    color_t ring = RGBA32(255, 240, 80, 255);
    kiln_gui_rect(cx - 1, cy - 1, CELL + 2, 1, ring);
    kiln_gui_rect(cx - 1, cy + CELL, CELL + 2, 1, ring);
    kiln_gui_rect(cx - 1, cy - 1, 1, CELL + 2, ring);
    kiln_gui_rect(cx + CELL, cy - 1, 1, CELL + 2, ring);

    /* The palette, with the active entry ringed. */
    int py = CANVAS_Y + KILN_VOXATLAS_TILE * CELL + 8;
    for (int i = 0; i < KILN_VOXATLAS_COLOURS; i++) {
        int px = CANVAS_X + i * SWATCH;
        if (i == 0) {
            kiln_gui_rect(px, py, SWATCH - 1, SWATCH - 1, RGBA32(60, 60, 60, 255));
            kiln_gui_text(px + 2, py + 1, RGBA32(160, 160, 160, 255), "-");
        } else {
            kiln_gui_rect(px, py, SWATCH - 1, SWATCH - 1,
                         tlut_to_gui(f->atlas.tlut[st][i]));
        }
        if (i == f->paint_colour) {
            kiln_gui_rect(px - 1, py - 2, SWATCH + 1, 1, ring);
            kiln_gui_rect(px - 1, py + SWATCH - 1, SWATCH + 1, 1, ring);
        }
    }

    kiln_gui_text(CANVAS_X, CANVAS_Y - 12, RGBA32(255, 210, 70, 255),
                 "tile %d = blk %d   col %d   %s",
                 f->paint_tile, f->paint_tile + 1, f->paint_colour,
                 f->paint_veiled ? "VEILED" : "cold");
    /* The bindings are printed ONCE, by forge_hud_draw's per-mode help line.
     * This panel used to print its own two lines as well and they landed on top
     * of it — an overlay that overlaps its own text is worse than a terser one. */
    kiln_gui_text(CANVAS_X, py + SWATCH + 4, RGBA32(140, 140, 140, 255),
                 "C-lr col  C-ud tile");
}
