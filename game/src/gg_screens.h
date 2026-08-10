// SPDX-License-Identifier: MPL-2.0
//
// gg_screens.h — the whole-game screen state machine.
//
// Everything before Phase 4 booted straight into an auto-advancing board
// demo. A real match has a shape around that: a title, a character pick
// per player, a board pick, the match itself, and a results table you can
// leave to start another one. This module owns that shape; gg_hud.c owns
// what the PLAY screen draws.
//
// ── One app struct, no globals ─────────────────────────────────────────
// GGApp holds every screen's state, and main() owns exactly one on the
// stack. Screens are an enum, not function pointers into separate
// modules with their own statics: the screens all read and write the same
// match configuration (who picked which goblin, which board, how many
// rounds), and threading that through callbacks would just be the same
// struct with more indirection. A mini-game scene switcher (Phase 8) IS
// the case for callbacks, because a mini-game genuinely doesn't share
// state with the board — that's a different problem and gets a different
// mechanism.
//
// ── Player 1 drives the menus ──────────────────────────────────────────
// Character select is the one screen where every port matters, and even
// there it is sequential: the game asks player 1 to pick, then player 2,
// and so on, from whichever controller is being passed around. Four
// simultaneous cursors would need four menu structs and a "everyone
// ready" gate for a screen that appears once per match. Sequential is
// what most N64 party games actually do and it is a quarter of the state.

#ifndef GG_SCREENS_H
#define GG_SCREENS_H

#include <m64/m64_widget.h>
#include <m64/m64_input.h>
#include <m64/m64_camera.h>
#include <m64/m64_engine.h>

#include "gg_types.h"
#include "gg_turn.h"
#include "gg_boards.h"

typedef enum {
    GG_SCREEN_TITLE = 0,
    GG_SCREEN_CHAR_SELECT,
    GG_SCREEN_BOARD_SELECT,
    GG_SCREEN_PLAY,
    GG_SCREEN_RESULTS,
} GGScreen;

typedef struct {
    GGScreen        screen;
    M64WidgetStyle  style;
    M64Menu         menu;        // the active screen's list

    // Match configuration, filled in by the select screens.
    uint8_t  goblin[GG_PLAYERS]; // index into gg_goblins per player
    uint8_t  picking;            // which player is choosing (CHAR_SELECT)
    uint8_t  board;              // index into gg_board_defs
    uint16_t rounds;             // resolved from the chosen board

    // Results, sorted best-first by gg_screens_rank.
    int   order[GG_PLAYERS];

    // Screen-transition banner ("ROUND 3", "DANK WINS"). `banner_t`
    // counts down in seconds; 0 means no banner.
    char  banner[24];
    float banner_t;
    float banner_total;

    // Whether the match auto-plays (A held / auto mode) or steps on input.
    uint8_t auto_play;
    float   step_timer;
} GGApp;

// Colour swatch per player seat, used by the HUD strip and results table.
extern const color_t gg_player_tint[GG_PLAYERS];

void gg_app_init(GGApp *app);

// Show a banner for `seconds`. Replaces any banner already up — the most
// recent thing that happened is the thing worth reading.
void gg_app_banner(GGApp *app, float seconds, const char *text);

// Advance the active screen one frame. `in` is player 1's input (the menu
// driver — see the file comment). Owns the camera because the transitions
// are what change camera mode: entering PLAY pushes M64_CAM_BOARD and fits
// it to the chosen board, leaving PLAY pops back. Returns the screen after
// the update, so main() can react to a transition without re-reading
// app->screen.
GGScreen gg_screens_update(GGApp *app, const M64Input *in, GGTurnState *turn,
                           M64Board *board, GGPlayer *players,
                           M64Camera *cam, const M64Scene *scene, float dt);

// Draw the active screen's 2D layer. Must be called inside the
// m64_gui_begin/end bracket. PLAY delegates to gg_hud_draw.
void gg_screens_draw(const GGApp *app, const M64Scene *scene,
                     const GGTurnState *turn, const M64Board *board,
                     const GGPlayer *players, int screen_w, int screen_h);

// Sort `order` best-first by bud count. Ties break toward the lower seat
// number, which is arbitrary but has to be *something* deterministic —
// the results table shows a strict ranking.
void gg_screens_rank(GGApp *app, const GGPlayer *players);

#endif // GG_SCREENS_H
