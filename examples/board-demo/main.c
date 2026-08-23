// SPDX-License-Identifier: MIT
//
// board-demo: the four Phase 1 engine primitives exercised in one loop.
//
//   kiln_rng    — seeded once at boot, drives every random draw
//   kiln_dice   — a standard 1..6 die, rolled per turn
//   kiln_board  — a 10-node branching path with one fork
//   kiln_turn   — 4 players, 5 rounds, ROLL→MOVE→LAND→EVENT→END per turn
//
// The HUD prints each player's position + point count and a rolling log of
// the last few rolls so the state machine is auditable. The board is drawn
// as a top-down 2D schematic in the GUI pass — no 3D, no assets — because
// the proof here is the topology and the turn state, not rendering.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_rng.h>
#include <kiln/kiln_dice.h>
#include <kiln/kiln_board.h>
#include <kiln/kiln_turn.h>

#define SCREEN_W 320
#define SCREEN_H 240
#define PLAYERS  4
#define ROUNDS   5

// A 10-node board: a loop with a fork at node 3 (long way / short cut) that
// rejoins at node 7. Spaces alternate GOOD/BAD/BONUS to exercise the
// per-type on-enter hook a real game would replace with its own per-type
// table; here the on-enter just awards points based on type.
static const KilnBoardNode nodes[10] = {
    { KILN_SPACE_START,    {{   0, 0,   0 }}, { 1 },         1 }, // 0 start
    { KILN_SPACE_GOOD,     {{  40, 0,   0 }}, { 2 },         1 }, // 1
    { KILN_SPACE_BAD,      {{  80, 0,   0 }}, { 3 },         1 }, // 2
    { KILN_SPACE_BONUS,    {{ 120, 0,   0 }}, { 4, 5 },      2 }, // 3 fork
    { KILN_SPACE_GOOD,     {{ 160, 0,  40 }}, { 6 },         1 }, // 4 long way
    { KILN_SPACE_GOOD,     {{ 160, 0, -40 }}, { 6 },         1 }, // 5 shortcut
    { KILN_SPACE_BAD,      {{ 200, 0,   0 }}, { 7 },         1 }, // 6 rejoin
    { KILN_SPACE_BONUS,    {{ 240, 0,   0 }}, { 8 },         1 }, // 7
    { KILN_SPACE_GOOD,     {{ 280, 0,   0 }}, { 9 },         1 }, // 8
    { KILN_SPACE_GOOD,     {{ 320, 0,   0 }}, { 0 },         1 }, // 9 back to start
};

static int16_t token_node[PLAYERS];
static int     points[PLAYERS];
static int     last_roll = 0;
static int     last_player = 0;
static char    log_lines[6][40];
static int     log_head = 0;

static void log_push(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(log_lines[log_head], sizeof(log_lines[0]), fmt, ap);
    va_end(ap);
    log_head = (log_head + 1) % 6;
}

// Award points on landing. The space's effect is game-side logic — this is
// the stub a real game would replace with its own per-type table.
static int award_for_space(KilnSpaceType t)
{
    switch (t) {
        case KILN_SPACE_GOOD:     return 3;
        case KILN_SPACE_BAD:      return -2;
        case KILN_SPACE_BONUS:    return 5;
        case KILN_SPACE_SHORTCUT: return 1;
        case KILN_SPACE_MINIGAME: return 4;
        case KILN_SPACE_TRADE:    return 0;
        default:                 return 0;
    }
}

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();

    KilnBoard board;
    kiln_board_init(&board, nodes, 10, 0);

    KilnRng rng;
    kiln_rng_seed(&rng, 0xC0FFEE);

    KilnDice die;
    kiln_dice_init_uniform(&die, 6);

    KilnTurn turn;
    kiln_turn_init(&turn, PLAYERS, ROUNDS);

    for (int p = 0; p < PLAYERS; p++) {
        token_node[p] = board.start_node;
        points[p] = 0;
    }

    // Auto-advance: one state transition per frame. A real game would gate
    // ROLL on a button press; here we just let it run so the loop is
    // observable without input.
    for (;;) {
        joypad_poll();

        int cur = turn.player;

        switch (turn.phase) {
            case KILN_PHASE_ROLL: {
                last_roll = kiln_dice_roll(&die, &rng);
                last_player = cur;
                log_push("P%d rolled %d", cur + 1, last_roll);
                kiln_turn_advance_phase(&turn);  // -> MOVE
                break;
            }
            case KILN_PHASE_MOVE: {
                for (int s = 0; s < last_roll; s++) {
                    // At a fork, pick branch 0 (long way). A real game would
                    // replace this with player-choice logic.
                    uint8_t branch = 0;
                    token_node[cur] = kiln_board_step(&board, token_node[cur], branch);
                }
                kiln_turn_advance_phase(&turn);  // -> LAND
                break;
            }
            case KILN_PHASE_LAND: {
                KilnSpaceType st = board.nodes[token_node[cur]].type;
                int award = award_for_space(st);
                points[cur] += award;
                if (points[cur] < 0) points[cur] = 0;
                log_push("P%d landed %s (%+d)", cur + 1,
                         st == KILN_SPACE_GOOD  ? "GOOD" :
                         st == KILN_SPACE_BAD   ? "BAD " :
                         st == KILN_SPACE_BONUS ? "BNUS" : "????",
                         award);
                kiln_turn_advance_phase(&turn);  // -> EVENT
                break;
            }
            case KILN_PHASE_EVENT:
                // No scheduled events in this demo; fall through to END.
                kiln_turn_advance_phase(&turn);
                break;
            case KILN_PHASE_END:
                kiln_turn_advance_phase(&turn);  // -> next player or round
                break;
            default:
                kiln_turn_advance_phase(&turn);
                break;
        }

        // ── render ──────────────────────────────────────────────────
        kiln_frame_begin();
        // No 3D scene in this demo; kiln_scene_begin still attaches the
        // framebuffer + Z-buffer the GUI pass needs.
        KilnScene scene;
        kiln_scene_init(&scene);
        scene.cam_pos = (fm_vec3_t){{ 0, 0, -10 }};
        kiln_scene_update(&scene);
        kiln_scene_begin(&scene);

        kiln_gui_begin();

        // Title + round
        kiln_gui_panel(8, 8, SCREEN_W - 16, 24,
                      RGBA32(10, 10, 24, 200), RGBA32(0, 245, 212, 255));
        kiln_gui_text(14, 16, RGBA32(0, 245, 212, 255),
                     "BOARD DEMO   round %d/%d   phase %d",
                     turn.round, ROUNDS, (int)turn.phase);

        // Player table
        kiln_gui_panel(8, 40, 200, 96,
                      RGBA32(10, 10, 24, 200), RGBA32(139, 92, 246, 255));
        for (int p = 0; p < PLAYERS; p++) {
            color_t c = (p == cur)
                ? RGBA32(0, 245, 212, 255)
                : RGBA32(232, 232, 240, 255);
            kiln_gui_text(14, 50 + p * 14, c,
                         "P%d  node %2d  pts  %3d%s",
                         p + 1, (int)token_node[p], points[p],
                         p == cur ? "  <- turn" : "");
        }

        // Rolling log
        kiln_gui_panel(216, 40, 96, 96,
                      RGBA32(10, 10, 24, 200), RGBA32(255, 200, 80, 255));
        kiln_gui_text(222, 48, RGBA32(255, 200, 80, 255), "log");
        for (int i = 0; i < 6; i++) {
            int idx = (log_head + i) % 6;
            kiln_gui_text(222, 60 + i * 12, RGBA32(200, 200, 200, 255),
                         "%s", log_lines[idx]);
        }

        // Bottom: last roll
        kiln_gui_panel(8, SCREEN_H - 32, SCREEN_W - 16, 24,
                      RGBA32(10, 10, 24, 200), RGBA32(139, 92, 246, 255));
        kiln_gui_text(14, SCREEN_H - 22, RGBA32(232, 232, 240, 255),
                     "P%d last roll: %d   total points: %d",
                     last_player + 1, last_roll,
                     points[0] + points[1] + points[2] + points[3]);

        kiln_gui_end();
        kiln_frame_end();
    }
}