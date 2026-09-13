// SPDX-License-Identifier: MIT
//
// board-demo: a four-player party-game board, played by the CPU, in 3D.
//
//   kiln_rng     seeded once per game; every roll and every fork choice
//   kiln_dice    a 1..6 die, and a two-faced coin for the fork
//   kiln_board   a 12-space loop with a fork: the long way round, or a shortcut
//   kiln_turn    4 players, 5 rounds, ROLL -> MOVE -> LAND -> EVENT -> END
//   kiln_camera  KILN_CAM_BOARD: frames the whole board, leans in on a move
//   kiln_widget  the rolling die, the per-player strip, banners, results
//
// Phases are paced by time rather than advancing one per frame, so a turn is
// something you can watch: the die spins, the token hops space to space, the
// space's effect is called out, and after five rounds the scores are ranked
// and a new game begins with a new seed. kiln_turn keeps rotating players past
// its last round, so the end of the game is detected here.
//
//   A   reveal the roll now      START   new game
//
// Jumps: .#board-demo-fork holds on a token taking the fork;
// .#board-demo-results holds on the final scores.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_rng.h>
#include <kiln/kiln_dice.h>
#include <kiln/kiln_board.h>
#include <kiln/kiln_turn.h>
#include <kiln/kiln_camera.h>
#include <kiln/kiln_widget.h>
#include <kiln/kiln_prim.h>

#include <stdio.h>

enum { JUMP_NONE, JUMP_FORK, JUMP_RESULTS };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W 320
#define SCREEN_H 240
#define PLAYERS  4
#define ROUNDS   5
#define NODES    12
#define MAX_EDGES 24

#define T_ROLL_SPIN 0.9f
#define T_ROLL_HOLD 1.5f
#define T_HOP       0.30f
#define T_LAND      1.1f

// An oval loop. Space 3 forks: the long way (4 -> 6) or the shortcut (5);
// both rejoin at 7.
static const KilnBoardNode NODE_TABLE[NODES] = {
    { KILN_SPACE_START,    {{    0, 0, -100 }}, { 1 },    1 },
    { KILN_SPACE_GOOD,     {{   62, 0,  -88 }}, { 2 },    1 },
    { KILN_SPACE_BAD,      {{  104, 0,  -48 }}, { 3 },    1 },
    { KILN_SPACE_BONUS,    {{  116, 0,    6 }}, { 4, 5 }, 2 },
    { KILN_SPACE_GOOD,     {{  104, 0,   62 }}, { 6 },    1 },
    { KILN_SPACE_SHORTCUT, {{   48, 0,   30 }}, { 7 },    1 },
    { KILN_SPACE_GOOD,     {{   58, 0,   98 }}, { 7 },    1 },
    { KILN_SPACE_BONUS,    {{    0, 0,  106 }}, { 8 },    1 },
    { KILN_SPACE_BAD,      {{  -62, 0,   94 }}, { 9 },    1 },
    { KILN_SPACE_GOOD,     {{ -104, 0,   52 }}, { 10 },   1 },
    { KILN_SPACE_MINIGAME, {{ -116, 0,   -8 }}, { 11 },   1 },
    { KILN_SPACE_GOOD,     {{  -84, 0,  -70 }}, { 0 },    1 },
};

static const color_t PLAYER_TINT[PLAYERS] = {
    RGBA32(0xFF, 0x60, 0x60, 0xFF), RGBA32(0x60, 0xB0, 0xFF, 0xFF),
    RGBA32(0x70, 0xE0, 0x80, 0xFF), RGBA32(0xFF, 0xD0, 0x50, 0xFF),
};
static const char *PLAYER_NAME[PLAYERS] = { "Rufus", "Blue", "Moss", "Sunny" };

static uint32_t space_rgba(KilnSpaceType t)
{
    switch (t) {
    case KILN_SPACE_START:    return 0xF0F0F0FF;
    case KILN_SPACE_GOOD:     return 0x4C80E8FF;
    case KILN_SPACE_BAD:      return 0xE04848FF;
    case KILN_SPACE_BONUS:    return 0xF0C040FF;
    case KILN_SPACE_SHORTCUT: return 0x48C888FF;
    case KILN_SPACE_MINIGAME: return 0xB070F0FF;
    default:                  return 0x40C8C8FF;
    }
}

static int award_for_space(KilnSpaceType t)
{
    switch (t) {
    case KILN_SPACE_GOOD:     return 3;
    case KILN_SPACE_BAD:      return -2;
    case KILN_SPACE_BONUS:    return 5;
    case KILN_SPACE_SHORTCUT: return 1;
    case KILN_SPACE_MINIGAME: return 4;
    default:                  return 0;
    }
}

static const char *space_name(KilnSpaceType t)
{
    switch (t) {
    case KILN_SPACE_GOOD: return "GOOD"; case KILN_SPACE_BAD: return "BAD";
    case KILN_SPACE_BONUS: return "BONUS"; case KILN_SPACE_SHORTCUT: return "SHORTCUT";
    case KILN_SPACE_MINIGAME: return "MINIGAME"; case KILN_SPACE_START: return "START";
    default: return "?";
    }
}

// ── Game state ──────────────────────────────────────────────────────────
static KilnBoard g_board;
static KilnTurn g_turn;
static KilnRng g_rng;
static KilnDice g_die, g_coin;
static int16_t g_node[PLAYERS];
static int g_points[PLAYERS];
static uint32_t g_seed = 0xC0FFEE;

static float g_phase_t;          /* seconds in the current phase           */
static int g_roll;               /* the revealed face; 0 until revealed    */
static int g_steps_left;
static int16_t g_hop_from, g_hop_to;
static char g_banner[32];
static float g_banner_t;
static int g_game_over;
static float g_over_t;
static int g_forced_roll;        /* jump ROMs: the first roll's face       */
static int g_hold;               /* jump ROMs: freeze once the shot is set */

static void banner(const char *text, float secs)
{
    snprintf(g_banner, sizeof g_banner, "%s", text);
    g_banner_t = secs;
}

static void new_game(void)
{
    kiln_rng_seed(&g_rng, g_seed++);
    kiln_turn_init(&g_turn, PLAYERS, ROUNDS);
    for (int p = 0; p < PLAYERS; p++) { g_node[p] = g_board.start_node; g_points[p] = 0; }
    g_phase_t = 0; g_roll = 0; g_steps_left = 0;
    g_game_over = 0; g_over_t = 0;
    banner("ROUND 1", 1.2f);
}

static fm_vec3_t token_pos(int p, float t_hop)
{
    /* Four tokens share a space; spread them onto its corners. */
    const float ox = (p & 1) ? 5.0f : -5.0f, oz = (p & 2) ? 5.0f : -5.0f;
    fm_vec3_t a = g_board.nodes[g_node[p]].pos;
    if (p == g_turn.player && g_turn.phase == KILN_PHASE_MOVE && g_hop_to >= 0) {
        const fm_vec3_t from = g_board.nodes[g_hop_from].pos;
        const fm_vec3_t to = g_board.nodes[g_hop_to].pos;
        const float k = t_hop < 0 ? 0 : t_hop > 1 ? 1 : t_hop;
        a.v[0] = from.v[0] + (to.v[0] - from.v[0]) * k;
        a.v[2] = from.v[2] + (to.v[2] - from.v[2]) * k;
        a.v[1] = 14.0f * fm_sinf(k * 3.14159f);
    }
    return (fm_vec3_t){{ a.v[0] + ox, a.v[1] + 11.0f, a.v[2] + oz }};
}

/* One frame of the turn machine. Returns nothing; reads and writes g_*. */
static void step_game(const KilnInput *in, float dt)
{
    const int cur = g_turn.player;
    g_phase_t += dt;

    switch (g_turn.phase) {
    case KILN_PHASE_ROLL:
        if (g_roll == 0 && (g_phase_t >= T_ROLL_SPIN || (in->edges & KILN_BTN_A))) {
            g_roll = g_forced_roll ? g_forced_roll : kiln_dice_roll(&g_die, &g_rng);
            g_forced_roll = 0;
            g_phase_t = T_ROLL_SPIN;
        }
        if (g_roll && g_phase_t >= T_ROLL_HOLD) {
            g_steps_left = g_roll;
            g_hop_from = g_node[cur]; g_hop_to = -1;
            kiln_turn_advance_phase(&g_turn);     /* -> MOVE */
            g_phase_t = T_HOP;                    /* start the first hop now */
        }
        break;

    case KILN_PHASE_MOVE:
        if (g_phase_t >= T_HOP) {
            if (g_hop_to >= 0) g_node[cur] = g_hop_to;
            if (g_steps_left == 0) {
                g_hop_to = -1;
                const KilnSpaceType st = g_board.nodes[g_node[cur]].type;
                const int award = award_for_space(st);
                g_points[cur] += award;
                if (g_points[cur] < 0) g_points[cur] = 0;
                char text[32];
                snprintf(text, sizeof text, "%s %+d  %s", PLAYER_NAME[cur], award, space_name(st));
                banner(text, T_LAND);
                kiln_turn_advance_phase(&g_turn); /* -> LAND */
                g_phase_t = 0;
                break;
            }
            /* The fork is a coin flip, drawn from the same seeded stream. */
            uint8_t branch = 0;
            const KilnBoardNode *n = &g_board.nodes[g_node[cur]];
            if (n->next_count > 1) {
                branch = (uint8_t)(kiln_dice_roll(&g_coin, &g_rng) - 1);
                banner(branch ? "FORK: THE SHORTCUT" : "FORK: THE LONG WAY", 1.0f);
                if (KILN_JUMP == JUMP_FORK) g_hold = 1;
            }
            g_hop_from = g_node[cur];
            g_hop_to = kiln_board_step(&g_board, g_node[cur], branch);
            g_steps_left--;
            g_phase_t = 0;
        }
        break;

    case KILN_PHASE_LAND:
        if (g_phase_t >= T_LAND) { kiln_turn_advance_phase(&g_turn); g_phase_t = 0; }
        break;

    case KILN_PHASE_EVENT:
        kiln_turn_advance_phase(&g_turn);
        g_phase_t = 0;
        break;

    case KILN_PHASE_END:
    default:
        if (cur == PLAYERS - 1 && g_turn.round >= ROUNDS) {
            g_game_over = 1;
            banner("GAME OVER", 1.5f);
            break;
        }
        {
            const uint16_t round = g_turn.round;
            kiln_turn_advance_phase(&g_turn);    /* -> next player's ROLL */
            if (g_turn.round != round) {
                char text[16];
                snprintf(text, sizeof text, "ROUND %u", (unsigned)g_turn.round);
                banner(text, 1.2f);
            }
        }
        g_roll = 0;
        g_phase_t = 0;
        break;
    }
}

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    kiln_input_init();

    kiln_board_init(&g_board, NODE_TABLE, NODES, 0);
    kiln_dice_init_uniform(&g_die, 6);
    kiln_dice_init_uniform(&g_coin, 2);
    new_game();

    if (KILN_JUMP == JUMP_FORK) {
        /* One space before the fork with a roll of 2: the first hop reaches
         * space 3, the second takes the fork. */
        g_node[0] = 2;
        g_forced_roll = 2;
    } else if (KILN_JUMP == JUMP_RESULTS) {
        static const int P[PLAYERS] = { 23, 31, 17, 26 };
        for (int p = 0; p < PLAYERS; p++) g_points[p] = P[p];
        g_game_over = 1;
        g_hold = 1;
    }

    // ── The board, as geometry ──────────────────────────────────────────
    KilnPrim pads[NODES], strips[MAX_EDGES], table, token_body, token_cap[PLAYERS];
    KilnTransform pad_xf[NODES], strip_xf[MAX_EDGES], token_xf[PLAYERS], table_xf;
    int strip_count = 0;
    for (int i = 0; i < NODES; i++) {
        const uint32_t c = space_rgba(NODE_TABLE[i].type);
        kiln_prim_box(&pads[i], (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 13, 2, 13 }},
                      c, kiln_prim_shade(c, 0.6f), kiln_prim_shade(c, 0.3f));
        kiln_transform_init(&pad_xf[i]);
        pad_xf[i].pos = (fm_vec3_t){{ NODE_TABLE[i].pos.v[0], 2, NODE_TABLE[i].pos.v[2] }};
        for (int e = 0; e < NODE_TABLE[i].next_count && strip_count < MAX_EDGES; e++) {
            const fm_vec3_t a = NODE_TABLE[i].pos, b = NODE_TABLE[NODE_TABLE[i].next[e]].pos;
            const float dx = b.v[0] - a.v[0], dz = b.v[2] - a.v[2];
            const float len = fm_vec3_len(&(fm_vec3_t){{ dx, 0, dz }});
            kiln_prim_box(&strips[strip_count], (fm_vec3_t){{ 0, 0, 0 }},
                          (fm_vec3_t){{ len * 0.5f, 1, 3 }}, 0xB8C0D0FF, 0x707888FF, 0x404048FF);
            KilnTransform *t = &strip_xf[strip_count++];
            kiln_transform_init(t);
            t->pos = (fm_vec3_t){{ (a.v[0] + b.v[0]) * 0.5f, 0.5f, (a.v[2] + b.v[2]) * 0.5f }};
            /* A box's long axis is local +X; turning by atan2(-dz, dx) about
             * +Y lays it along the edge. */
            t->rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
            t->rot_angle = fm_atan2f(-dz, dx);
        }
    }
    kiln_prim_floor(&table, 170.0f, 17, kiln_prim_rgba(0x1E, 0x42, 0x30), kiln_prim_rgba(0x1A, 0x3A, 0x2A));
    kiln_transform_init(&table_xf);
    kiln_prim_box(&token_body, (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 4, 7, 4 }},
                  0xF0F0F0FF, 0xC8C8D0FF, 0x606068FF);
    for (int p = 0; p < PLAYERS; p++) {
        const uint32_t c = kiln_prim_rgba(PLAYER_TINT[p].r, PLAYER_TINT[p].g, PLAYER_TINT[p].b);
        kiln_prim_box(&token_cap[p], (fm_vec3_t){{ 0, 9, 0 }}, (fm_vec3_t){{ 5, 3, 5 }},
                      c, kiln_prim_shade(c, 0.7f), kiln_prim_shade(c, 0.4f));
        kiln_transform_init(&token_xf[p]);
        token_xf[p].rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
    }

    KilnScene scene;
    kiln_scene_init(&scene);
    kiln_prim_stage(&scene, RGBA32(0x10, 0x16, 0x24, 0xFF), 320.0f, 760.0f);
    scene.fov_deg = 60.0f;
    scene.near_z = 10.0f;
    scene.far_z = 760.0f;

    KilnCamera cam;
    kiln_camera_init(&cam);
    kiln_camera_push(&cam, KILN_CAM_BOARD);
    kiln_camera_set_board(&cam, (fm_vec3_t){{ 0, 0, 0 }}, 118.0f, 56.0f, scene.fov_deg);
    kiln_camera_set_board_spin(&cam, 0.10f);
    kiln_camera_snap_board(&cam, token_pos(0, 0));

    const KilnWidgetStyle st = kiln_widget_style_funky();

    for (;;) {
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);
        const float dt = 1.0f / 60.0f;
        kiln_widget_tick(dt);

        if (in->edges & KILN_BTN_START) { g_hold = 0; new_game(); }
        if (!g_hold) {
            if (g_game_over) {
                g_over_t += dt;
                if (g_over_t > 6.0f) new_game();
            } else {
                step_game(in, dt);
            }
            if (g_banner_t > 0) g_banner_t -= dt;
        }

        const int cur = g_turn.player;
        const float hop_k = g_phase_t / T_HOP;
        const fm_vec3_t focus = token_pos(cur, hop_k);
        kiln_camera_set_board_focus(&cam, (g_turn.phase == KILN_PHASE_MOVE && !g_game_over) ? 0.8f : 0.2f, 0);
        kiln_camera_update(&cam, focus, 0, dt);
        kiln_camera_apply(&cam, &scene);
        kiln_scene_update(&scene);

        // ── 3D ────────────────────────────────────────────────────────────
        kiln_frame_begin();
        kiln_scene_begin(&scene);
        table_xf.pos = (fm_vec3_t){{ 0, -0.5f, 0 }};
        kiln_transform_push(&table_xf); kiln_prim_draw(&table); kiln_transform_pop();
        for (int i = 0; i < strip_count; i++) {
            kiln_transform_push(&strip_xf[i]); kiln_prim_draw(&strips[i]); kiln_transform_pop();
        }
        for (int i = 0; i < NODES; i++) {
            /* The space under the moving token pulses up a little. */
            pad_xf[i].pos.v[1] = (i == g_node[cur] && !g_game_over) ? 3.0f + fm_sinf(kiln_widget_time() * 6) : 2.0f;
            kiln_transform_push(&pad_xf[i]); kiln_prim_draw(&pads[i]); kiln_transform_pop();
        }
        for (int p = 0; p < PLAYERS; p++) {
            token_xf[p].pos = token_pos(p, p == cur ? hop_k : 0);
            token_xf[p].rot_angle = p == cur ? kiln_widget_time() * 2.0f : 0.0f;
            kiln_transform_push(&token_xf[p]);
            kiln_prim_draw(&token_body);
            kiln_prim_draw(&token_cap[p]);
            kiln_transform_pop();
        }

        // ── 2D ────────────────────────────────────────────────────────────
        kiln_gui_begin();

        KilnPlayerSlot slots[PLAYERS];
        for (int p = 0; p < PLAYERS; p++) {
            slots[p] = (KilnPlayerSlot){
                .name = PLAYER_NAME[p], .note = NULL, .score = g_points[p],
                .charge = (float)g_node[p] / (float)(NODES - 1), .tint = PLAYER_TINT[p],
                .active = (uint8_t)(p == cur && !g_game_over), .ready = 0,
            };
        }

        if (g_game_over) {
            int order[PLAYERS] = { 0, 1, 2, 3 };
            for (int i = 1; i < PLAYERS; i++)
                for (int j = i; j > 0 && g_points[order[j]] > g_points[order[j - 1]]; j--) {
                    const int tmp = order[j]; order[j] = order[j - 1]; order[j - 1] = tmp;
                }
            kiln_widget_results(60, 52, 200, "FINAL SCORES", slots, order, PLAYERS, &st);
            kiln_gui_text(92, SCREEN_H - 14, RGBA32(0x90, 0x98, 0xB0, 0xFF),
                          g_hold ? "START: new game" : "new game shortly");
        } else {
            kiln_widget_hud_strip(8, 8, 140, slots, PLAYERS, &st);
            char round[16];
            snprintf(round, sizeof round, "ROUND %u/%d", (unsigned)g_turn.round, ROUNDS);
            kiln_widget_banner(SCREEN_W - 100, 8, 92, 18, round, 1.0f, &st);

            if (g_turn.phase == KILN_PHASE_ROLL) {
                kiln_widget_dice(SCREEN_W / 2 - 18, SCREEN_H - 70, 36, g_roll ? g_roll : 1,
                                 g_roll == 0, g_phase_t, &st);
            }
            kiln_gui_text(8, SCREEN_H - 6, RGBA32(0x90, 0x98, 0xB0, 0xFF), "A roll now   START new game");
        }

        if (g_banner_t > 0 || g_hold) {
            const float fade = g_hold ? 1.0f : (g_banner_t > 0.25f ? 1.0f : g_banner_t * 4.0f);
            if (g_banner[0] && !(g_game_over && KILN_JUMP == JUMP_RESULTS))
                kiln_widget_banner(40, 100, 240, 24, g_banner, fade, &st);
        }

        kiln_gui_end();
        kiln_frame_end();
    }
}
