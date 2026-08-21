// SPDX-License-Identifier: MPL-2.0
//
// Ganja Goblin — a 4-player N64-style party game on the Kiln engine.
//
// Phase 4: the game has a shape around the board loop. Title, character
// select (four goblins, one seat at a time), board select (Ganja Grove or
// the short Blitz Harvest), the match itself under a board camera that
// orbits the whole board and tightens onto whoever is moving, and a
// results table with a rematch option. Forks stop the token and ask.
//
// main() owns the frame loop and nothing else: input polling, the camera
// update, and the two-pass bracket. Which screen is up, what it draws, and
// every match rule live in gg_screens / gg_hud / gg_turn.
//
// What's NOT here yet: 3D goblin models (deferred — needs Blender
// authoring), items + status effects (Phase 5), audio (Phase 6), particle
// VFX (Phase 7), mini-games (Phase 8). The board draws as camera-projected
// markers rather than meshes — see gg_hud.h for why that is a choice and
// not a stub.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_camera.h>
#include <kiln/kiln_inventory.h>
#include <kiln/kiln_widget.h>

#include "gg_types.h"
#include "gg_boards.h"
#include "gg_buds.h"
#include "gg_turn.h"
#include "gg_goblins.h"
#include "gg_specials.h"
#include "gg_screens.h"
#include "gg_hud.h"

#define SCREEN_W 320
#define SCREEN_H 240

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    kiln_input_init();

    KilnScene scene;
    kiln_scene_init(&scene);
    scene.far_z = 2000.0f;   // board world units run to ~300 across
    scene.clear_color = RGBA32(12, 8, 22, 255);

    KilnBoard board;
    gg_boards_load(&board, 0);

    GGTurnState turn;
    gg_turn_init(&turn, gg_board_defs[0].rounds);

    GGPlayer players[GG_PLAYERS];
    for (int p = 0; p < GG_PLAYERS; p++) {
        players[p].node = board.start_node;
        players[p].buds = GG_STARTING_BUDS;
        players[p].status = GG_STATUS_NONE;
        players[p].status_turns = 0;
        players[p].pending_move_bonus = 0;
        players[p].pending_bud_bonus = 0;
        players[p].turns_played = 0;
        kiln_inventory_init(&players[p].inv);
    }
    gg_specials_set_players(players, GG_PLAYERS);

    KilnCamera cam;
    kiln_camera_init(&cam);

    GGApp app;
    gg_app_init(&app);

    uint32_t last_ticks = get_ticks();

    for (;;) {
        uint32_t now = get_ticks();
        float dt = (float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND;
        last_ticks = now;
        // Clamp the first frame (and any hitch): a multi-second dt fed to
        // the camera dampers snaps them, and fed to the auto-play timer
        // burns several turns in one frame.
        if (dt > 0.1f) dt = 0.1f;

        kiln_input_update();
        // The widget layer's sway, lean and per-item drift all hang off one
        // clock; nothing on a menu screen holds still without it.
        kiln_widget_tick(dt);
        const KilnInput *in = kiln_input_get(0);

        gg_screens_update(&app, in, &turn, &board, players, &cam, &scene, dt);

        // The camera follows the active token; on the menu screens there is
        // no token to follow, so it keeps whatever framing it had.
        if (app.screen == GG_SCREEN_PLAY || app.screen == GG_SCREEN_RESULTS) {
            const fm_vec3_t *tp =
                kiln_board_pos(&board, players[turn.turn.player].node);
            kiln_camera_update(&cam, *tp, cam.yaw, dt);
            kiln_camera_apply(&cam, &scene);
        }
        kiln_scene_update(&scene);

        kiln_frame_begin();
        kiln_scene_begin(&scene);

        kiln_gui_begin();
        gg_screens_draw(&app, &scene, &turn, &board, players,
                        SCREEN_W, SCREEN_H);
        kiln_gui_end();

        kiln_frame_end();
    }
}
