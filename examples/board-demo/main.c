// SPDX-License-Identifier: MPL-2.0
//
// board-demo: the four Phase 1 engine primitives exercised in one loop.
//
//   m64_rng    — seeded once at boot, drives every random draw
//   m64_dice   — a standard 1..6 die, rolled per turn
//   m64_board  — a 10-node branching path with one fork
//   m64_turn   — 4 players, 5 rounds, ROLL→MOVE→LAND→EVENT→END per turn
//
// The HUD prints each player's position + bud count and a rolling log of
// the last few rolls so the state machine is auditable. The board is drawn
// as a top-down 2D schematic in the GUI pass — no 3D, no assets — because
// the proof here is the topology and the turn state, not rendering.

#include <libdragon.h>
#include <m64/m64_engine.h>
#include <m64/m64_gui.h>
#include <m64/m64_rng.h>
#include <m64/m64_dice.h>
#include <m64/m64_board.h>
#include <m64/m64_turn.h>

#define SCREEN_W 320
#define SCREEN_H 240
#define PLAYERS  4
#define ROUNDS   5

// A 10-node board: a loop with a fork at node 3 (long way / short cut) that
// rejoins at node 7. Spaces alternate Grow/Dry/Spirit to exercise the
// per-type on-enter hook in gg_spaces (Phase 2); here the on-enter just
// awards buds based on type.
static const M64BoardNode nodes[10] = {
    { M64_SPACE_START,    {{   0, 0,   0 }}, { 1 },         1 }, // 0 start
    { M64_SPACE_GROW,     {{  40, 0,   0 }}, { 2 },         1 }, // 1
    { M64_SPACE_DRY,      {{  80, 0,   0 }}, { 3 },         1 }, // 2
    { M64_SPACE_SPIRIT,   {{ 120, 0,   0 }}, { 4, 5 },      2 }, // 3 fork
    { M64_SPACE_GROW,     {{ 160, 0,  40 }}, { 6 },         1 }, // 4 long way
    { M64_SPACE_GROW,     {{ 160, 0, -40 }}, { 6 },         1 }, // 5 shortcut
    { M64_SPACE_DRY,      {{ 200, 0,   0 }}, { 7 },         1 }, // 6 rejoin
    { M64_SPACE_SPIRIT,   {{ 240, 0,   0 }}, { 8 },         1 }, // 7
    { M64_SPACE_GROW,     {{ 280, 0,   0 }}, { 9 },         1 }, // 8
    { M64_SPACE_GROW,     {{ 320, 0,   0 }}, { 0 },         1 }, // 9 back to start
};

static int16_t token_node[PLAYERS];
static int     buds[PLAYERS];
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

// Award buds on landing. The space's effect is game-side logic — this is the
// stub Phase 2 will replace with gg_spaces.c's per-type table.
static int award_for_space(M64SpaceType t)
{
    switch (t) {
        case M64_SPACE_GROW:     return 3;
        case M64_SPACE_DRY:      return -2;
        case M64_SPACE_SPIRIT:   return 5;
        case M64_SPACE_SHORTCUT: return 1;
        case M64_SPACE_MINIGAME: return 4;
        case M64_SPACE_TRADE:    return 0;
        default:                 return 0;
    }
}

int main(void)
{
    m64_engine_init(RESOLUTION_320x240);
    joypad_init();

    M64Board board;
    m64_board_init(&board, nodes, 10, 0);

    M64Rng rng;
    m64_rng_seed(&rng, 0xC0FFEE);

    M64Dice die;
    m64_dice_init_uniform(&die, 6);

    M64Turn turn;
    m64_turn_init(&turn, PLAYERS, ROUNDS);

    for (int p = 0; p < PLAYERS; p++) {
        token_node[p] = board.start_node;
        buds[p] = 0;
    }

    // Auto-advance: one state transition per frame. A real game would gate
    // ROLL on a button press; here we just let it run so the loop is
    // observable without input.
    for (;;) {
        joypad_poll();

        int cur = turn.player;

        switch (turn.phase) {
            case M64_PHASE_ROLL: {
                last_roll = m64_dice_roll(&die, &rng);
                last_player = cur;
                log_push("P%d rolled %d", cur + 1, last_roll);
                m64_turn_advance_phase(&turn);  // -> MOVE
                break;
            }
            case M64_PHASE_MOVE: {
                for (int s = 0; s < last_roll; s++) {
                    // At a fork, pick branch 0 (long way). Phase 2's
                    // gg_turn.c will replace this with player-choice logic.
                    uint8_t branch = 0;
                    token_node[cur] = m64_board_step(&board, token_node[cur], branch);
                }
                m64_turn_advance_phase(&turn);  // -> LAND
                break;
            }
            case M64_PHASE_LAND: {
                M64SpaceType st = board.nodes[token_node[cur]].type;
                int award = award_for_space(st);
                buds[cur] += award;
                if (buds[cur] < 0) buds[cur] = 0;
                log_push("P%d landed %s (%+d)", cur + 1,
                         st == M64_SPACE_GROW ? "GROW" :
                         st == M64_SPACE_DRY  ? "DRY " :
                         st == M64_SPACE_SPIRIT ? "SPRT" : "????",
                         award);
                m64_turn_advance_phase(&turn);  // -> EVENT
                break;
            }
            case M64_PHASE_EVENT:
                // No scheduled events in this demo; fall through to END.
                m64_turn_advance_phase(&turn);
                break;
            case M64_PHASE_END:
                m64_turn_advance_phase(&turn);  // -> next player or round
                break;
            default:
                m64_turn_advance_phase(&turn);
                break;
        }

        // ── render ──────────────────────────────────────────────────
        m64_frame_begin();
        // No 3D scene in this demo; m64_scene_begin still attaches the
        // framebuffer + Z-buffer the GUI pass needs.
        M64Scene scene;
        m64_scene_init(&scene);
        scene.cam_pos = (fm_vec3_t){{ 0, 0, -10 }};
        m64_scene_update(&scene);
        m64_scene_begin(&scene);

        m64_gui_begin();

        // Title + round
        m64_gui_panel(8, 8, SCREEN_W - 16, 24,
                      RGBA32(10, 10, 24, 200), RGBA32(0, 245, 212, 255));
        m64_gui_text(14, 16, RGBA32(0, 245, 212, 255),
                     "BOARD DEMO   round %d/%d   phase %d",
                     turn.round, ROUNDS, (int)turn.phase);

        // Player table
        m64_gui_panel(8, 40, 200, 96,
                      RGBA32(10, 10, 24, 200), RGBA32(139, 92, 246, 255));
        for (int p = 0; p < PLAYERS; p++) {
            color_t c = (p == cur)
                ? RGBA32(0, 245, 212, 255)
                : RGBA32(232, 232, 240, 255);
            m64_gui_text(14, 50 + p * 14, c,
                         "P%d  node %2d  buds %3d%s",
                         p + 1, (int)token_node[p], buds[p],
                         p == cur ? "  <- turn" : "");
        }

        // Rolling log
        m64_gui_panel(216, 40, 96, 96,
                      RGBA32(10, 10, 24, 200), RGBA32(255, 200, 80, 255));
        m64_gui_text(222, 48, RGBA32(255, 200, 80, 255), "log");
        for (int i = 0; i < 6; i++) {
            int idx = (log_head + i) % 6;
            m64_gui_text(222, 60 + i * 12, RGBA32(200, 200, 200, 255),
                         "%s", log_lines[idx]);
        }

        // Bottom: last roll
        m64_gui_panel(8, SCREEN_H - 32, SCREEN_W - 16, 24,
                      RGBA32(10, 10, 24, 200), RGBA32(139, 92, 246, 255));
        m64_gui_text(14, SCREEN_H - 22, RGBA32(232, 232, 240, 255),
                     "P%d last roll: %d   total buds: %d",
                     last_player + 1, last_roll,
                     buds[0] + buds[1] + buds[2] + buds[3]);

        m64_gui_end();
        m64_frame_end();
    }
}