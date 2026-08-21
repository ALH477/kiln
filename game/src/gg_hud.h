// SPDX-License-Identifier: MPL-2.0
//
// gg_hud.h — what the PLAY screen draws.
//
// Split from gg_screens.c because the in-match overlay is most of the
// game's drawing code and none of its control flow: gg_screens owns "which
// screen and when", this file owns "what the board looks like".
//
// ── The board is drawn projected, not modelled ─────────────────────────
// Every node and token is a 2D marker placed at the screen position
// kiln_scene_project reports for its world position. That is not a
// placeholder for "real" 3D geometry so much as a different, cheaper
// answer to the same question: a board space is a flat disc read from
// above, and drawing it as a screen-space marker costs one projection and
// one rdpq rect instead of a model, a matrix, and a Tiny3D draw call — for
// something the player only ever sees head-on.
//
// It is genuinely camera-driven despite being 2D: the markers move,
// spread and converge as KILN_CAM_BOARD orbits and focuses, because their
// positions come from the same view basis the 3D pass would use. When
// goblin models land, the tokens become real meshes and the node markers
// stay exactly as they are.

#ifndef GG_HUD_H
#define GG_HUD_H

#include "gg_screens.h"

// The in-match overlay: board schematic, per-player strip, dice, log,
// banner. Call inside the kiln_gui_begin/end bracket.
void gg_hud_draw(const GGApp *app, const KilnScene *scene,
                 const GGTurnState *turn, const KilnBoard *board,
                 const GGPlayer *players, int screen_w, int screen_h);

// Just the board + tokens, without the panels. Broken out because the
// results screen draws the board behind its table.
void gg_hud_draw_board(const KilnScene *scene, const KilnBoard *board,
                       const GGPlayer *players, int active_player,
                       const KilnWidgetStyle *st, int screen_w, int screen_h);

// Fill a KilnPlayerSlot array from the live game state — name, buds,
// charge, status note, seat tint. Shared by the HUD strip and the results
// table so the two cannot disagree about what a player is called.
void gg_hud_fill_slots(KilnPlayerSlot *slots, const GGApp *app,
                       const GGTurnState *turn, const GGPlayer *players);

#endif // GG_HUD_H
