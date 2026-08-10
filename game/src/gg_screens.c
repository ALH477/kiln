// SPDX-License-Identifier: MPL-2.0

#include "gg_screens.h"
#include "gg_hud.h"
#include "gg_goblins.h"
#include "gg_specials.h"
#include "gg_buds.h"

#include <fmath.h>
#include <string.h>
#include <stdio.h>

// Seat colours. Chosen to stay apart on a CRT at 320x240: green, magenta,
// amber, cyan — four hues from four different parts of the wheel, none of
// them the dark violet the panels use.
const color_t gg_player_tint[GG_PLAYERS] = {
    { .r =   0, .g = 245, .b = 120, .a = 255 },
    { .r = 255, .g =  90, .b = 190, .a = 255 },
    { .r = 255, .g = 190, .b =  60, .a = 255 },
    { .r =  90, .g = 200, .b = 255, .a = 255 },
};

#define SCREEN_W 320
#define SCREEN_H 240

// Auto-play pace. One phase every 0.35 s is slow enough to read the log
// line it produces and fast enough that a 10-round match is not a chore.
#define AUTO_STEP_SECS 0.35f

static const char *const k_title_items[] = {
    "START MATCH",
    "AUTO DEMO",
    "HOW TO PLAY",
};
#define TITLE_ITEM_COUNT 3

void gg_app_init(GGApp *app)
{
    memset(app, 0, sizeof(*app));
    app->screen = GG_SCREEN_TITLE;
    app->style = m64_widget_style_funky();
    m64_menu_init(&app->menu, TITLE_ITEM_COUNT, 0);
    app->board = 0;
    app->rounds = gg_board_defs[0].rounds;
    for (int p = 0; p < GG_PLAYERS; p++) app->goblin[p] = (uint8_t)p;
    for (int p = 0; p < GG_PLAYERS; p++) app->order[p] = p;
}

void gg_app_banner(GGApp *app, float seconds, const char *text)
{
    snprintf(app->banner, sizeof(app->banner), "%s", text ? text : "");
    app->banner_t = seconds;
    app->banner_total = seconds;
}

void gg_screens_rank(GGApp *app, const GGPlayer *players)
{
    for (int i = 0; i < GG_PLAYERS; i++) app->order[i] = i;
    // Insertion sort: four elements, and it is stable, which is what makes
    // the "ties break toward the lower seat" rule fall out for free.
    for (int i = 1; i < GG_PLAYERS; i++) {
        int v = app->order[i];
        int j = i - 1;
        while (j >= 0 && players[app->order[j]].buds < players[v].buds) {
            app->order[j + 1] = app->order[j];
            j--;
        }
        app->order[j + 1] = v;
    }
}

/** Start a match with the current app configuration. */
static void begin_match(GGApp *app, GGTurnState *turn, M64Board *board,
                        GGPlayer *players, M64Camera *cam,
                        const M64Scene *scene)
{
    gg_boards_load(board, app->board);
    app->rounds = gg_board_defs[app->board].rounds;
    gg_turn_restart(turn, app->rounds);

    for (int p = 0; p < GG_PLAYERS; p++) {
        players[p].node = board->start_node;
        players[p].buds = GG_STARTING_BUDS;
        players[p].status = GG_STATUS_NONE;
        players[p].status_turns = 0;
        players[p].pending_move_bonus = 0;
        players[p].pending_bud_bonus = 0;
        players[p].turns_played = 0;
        m64_inventory_init(&players[p].inv);
        m64_char_assign(&turn->chars[p], &gg_goblins[app->goblin[p]]);
    }
    gg_specials_set_players(players, GG_PLAYERS);

    // Fit the board camera to the board's own AABB. The bounding sphere's
    // radius is the half-diagonal, and the +8% keeps the outermost node
    // markers off the screen edge rather than exactly on it.
    fm_vec3_t c = {{
        (board->aabb_min.v[0] + board->aabb_max.v[0]) * 0.5f,
        (board->aabb_min.v[1] + board->aabb_max.v[1]) * 0.5f,
        (board->aabb_min.v[2] + board->aabb_max.v[2]) * 0.5f,
    }};
    fm_vec3_t half = {{
        (board->aabb_max.v[0] - board->aabb_min.v[0]) * 0.5f,
        (board->aabb_max.v[1] - board->aabb_min.v[1]) * 0.5f,
        (board->aabb_max.v[2] - board->aabb_min.v[2]) * 0.5f,
    }};
    float radius = fm_vec3_len(&half) * 1.08f;

    m64_camera_set_board(cam, c, radius, 55.0f, scene->fov_deg);
    m64_camera_set_board_spin(cam, 0.12f);   // slow drift, the doc's "wobble"
    m64_camera_set_board_focus(cam, 0.0f, 2.0f);
    if (cam->mode != M64_CAM_BOARD) m64_camera_push(cam, M64_CAM_BOARD);
    m64_camera_snap_board(cam, c);

    app->screen = GG_SCREEN_PLAY;
    app->step_timer = 0.0f;
    gg_app_banner(app, 1.6f, gg_board_defs[app->board].name);
}

/** Common cursor movement for a vertical menu: D-pad edges plus stick. */
static int menu_nav(M64Menu *m, const M64Input *in, const uint8_t *enabled,
                    int count)
{
    int delta = 0;
    if (in->edges & M64_BTN_DD) delta += 1;
    if (in->edges & M64_BTN_DU) delta -= 1;
    // Stick: treat a fresh push past half deflection as one step. Without
    // the edge check on the D-pad equivalent this would repeat every frame,
    // so the stick is deliberately edge-gated the same way — held-to-repeat
    // is a comfort feature for long lists, and none of these are long.
    static int stick_latched;
    if (in->stick_y > 0.5f)      { if (!stick_latched) { delta -= 1; stick_latched = 1; } }
    else if (in->stick_y < -0.5f) { if (!stick_latched) { delta += 1; stick_latched = 1; } }
    else stick_latched = 0;

    if (delta == 0) return 0;
    m64_menu_move_enabled(m, delta, enabled, count);
    return 1;
}

/** Which goblins are still unpicked, for the character-select list. */
static void goblin_availability(const GGApp *app, uint8_t *out)
{
    for (int g = 0; g < GG_GOBLIN_COUNT; g++) out[g] = 1;
    for (int p = 0; p < app->picking; p++) out[app->goblin[p]] = 0;
}

static void update_title(GGApp *app, const M64Input *in)
{
    menu_nav(&app->menu, in, NULL, TITLE_ITEM_COUNT);

    if (!(in->edges & M64_BTN_A)) return;

    switch (app->menu.cursor) {
        case 0:  // START MATCH
        case 1:  // AUTO DEMO
            app->auto_play = (app->menu.cursor == 1);
            app->picking = 0;
            app->screen = GG_SCREEN_CHAR_SELECT;
            m64_menu_init(&app->menu, GG_GOBLIN_COUNT, 0);
            break;
        case 2:  // HOW TO PLAY
            gg_app_banner(app, 3.0f, "A=PICK  B=BACK");
            break;
        default:
            break;
    }
}

static void update_char_select(GGApp *app, const M64Input *in)
{
    uint8_t avail[GG_GOBLIN_COUNT];
    goblin_availability(app, avail);

    // The cursor can be left on a goblin the previous player just took;
    // nudge it to the next free one so A is never a no-op.
    if (!avail[app->menu.cursor]) {
        m64_menu_move_enabled(&app->menu, 1, avail, GG_GOBLIN_COUNT);
    }
    menu_nav(&app->menu, in, avail, GG_GOBLIN_COUNT);

    if (in->edges & M64_BTN_B) {
        if (app->picking == 0) {
            app->screen = GG_SCREEN_TITLE;
            m64_menu_init(&app->menu, TITLE_ITEM_COUNT, 0);
        } else {
            app->picking--;
        }
        return;
    }

    if (!(in->edges & M64_BTN_A)) return;
    if (!avail[app->menu.cursor]) return;

    app->goblin[app->picking] = (uint8_t)app->menu.cursor;
    app->picking++;
    if (app->picking >= GG_PLAYERS) {
        app->screen = GG_SCREEN_BOARD_SELECT;
        m64_menu_init(&app->menu, GG_BOARD_COUNT, 0);
        app->menu.cursor = app->board;
    }
}

static void update_board_select(GGApp *app, const M64Input *in,
                                GGTurnState *turn, M64Board *board,
                                GGPlayer *players, M64Camera *cam,
                                const M64Scene *scene)
{
    menu_nav(&app->menu, in, NULL, GG_BOARD_COUNT);

    if (in->edges & M64_BTN_B) {
        app->picking = GG_PLAYERS - 1;
        app->screen = GG_SCREEN_CHAR_SELECT;
        m64_menu_init(&app->menu, GG_GOBLIN_COUNT, 0);
        return;
    }
    if (in->edges & M64_BTN_A) {
        app->board = (uint8_t)app->menu.cursor;
        begin_match(app, turn, board, players, cam, scene);
    }
}

static void update_play(GGApp *app, const M64Input *in, GGTurnState *turn,
                        M64Board *board, GGPlayer *players, M64Camera *cam,
                        float dt)
{
    if (turn->turn.phase == M64_PHASE_ROLL) turn->roll_anim_t += dt;

    // Camera: tighten onto the active token while it is walking or
    // resolving a space, pull back out otherwise. Both are requests to the
    // same damper, so the transition is smooth without any explicit
    // tweening here.
    int close = (turn->turn.phase == M64_PHASE_MOVE ||
                 turn->turn.phase == M64_PHASE_LAND);
    m64_camera_set_board_focus(cam, close ? 0.85f : 0.0f, 0.0f);

    if (in->edges & M64_BTN_START) {
        // Abort straight to results rather than to the title: a match you
        // walked away from still has a standing, and throwing it away is
        // more surprising than showing it.
        gg_screens_rank(app, players);
        app->screen = GG_SCREEN_RESULTS;
        m64_menu_init(&app->menu, 2, 0);
        return;
    }

    // A fork blocks MOVE until someone picks a direction. In auto-play
    // nobody will, so the demo takes the first branch after a beat.
    if (turn->awaiting_branch) {
        const M64BoardNode *nd = &board->nodes[players[turn->turn.player].node];
        if (app->auto_play) {
            app->step_timer += dt;
            if (app->step_timer >= AUTO_STEP_SECS) {
                app->step_timer = 0.0f;
                turn->branch_choice = 0;
            }
        } else {
            if (in->edges & (M64_BTN_DL | M64_BTN_CL)) turn->branch_choice = 0;
            if (in->edges & (M64_BTN_DR | M64_BTN_CR)) {
                turn->branch_choice = (int8_t)(nd->next_count > 1 ? 1 : 0);
            }
            if (in->stick_x < -0.6f) turn->branch_choice = 0;
            if (in->stick_x >  0.6f) {
                turn->branch_choice = (int8_t)(nd->next_count > 1 ? 1 : 0);
            }
        }
        if (turn->branch_choice < 0) return;   // still waiting
    }

    int step = 0;
    if (app->auto_play) {
        app->step_timer += dt;
        if (app->step_timer >= AUTO_STEP_SECS) {
            app->step_timer -= AUTO_STEP_SECS;
            step = 1;
        }
    } else {
        // Manual: A advances one phase. MOVE self-advances on a timer so a
        // six-space walk is one button press, not six.
        if (turn->turn.phase == M64_PHASE_MOVE) {
            app->step_timer += dt;
            if (app->step_timer >= 0.18f) { app->step_timer = 0.0f; step = 1; }
        } else if (in->edges & M64_BTN_A) {
            step = 1;
        }
    }
    if (!step) return;

    uint16_t round_before = turn->turn.round;
    // Catch the final END before it wraps: m64_turn clamps `round` at
    // max_rounds, so after the last player of the last round the round
    // number looks identical to the one before it. The transition has to be
    // sampled, not inferred afterwards.
    int was_final = (turn->turn.round >= app->rounds &&
                     m64_turn_round_will_advance(&turn->turn));

    gg_turn_step(turn, board, players);

    if (was_final) {
        gg_screens_rank(app, players);
        app->screen = GG_SCREEN_RESULTS;
        m64_menu_init(&app->menu, 2, 0);
        return;
    }
    if (turn->turn.round != round_before) {
        char msg[24];
        snprintf(msg, sizeof(msg), "ROUND %d", (int)turn->turn.round);
        gg_app_banner(app, 1.2f, msg);
    }
}

static void update_results(GGApp *app, const M64Input *in, GGTurnState *turn,
                           M64Board *board, GGPlayer *players, M64Camera *cam,
                           const M64Scene *scene)
{
    menu_nav(&app->menu, in, NULL, 2);
    if (!(in->edges & (M64_BTN_A | M64_BTN_START))) return;

    if (app->menu.cursor == 0) {
        // Rematch: same goblins, same board, fresh scores.
        begin_match(app, turn, board, players, cam, scene);
    } else {
        if (cam->mode == M64_CAM_BOARD) m64_camera_pop(cam);
        app->screen = GG_SCREEN_TITLE;
        m64_menu_init(&app->menu, TITLE_ITEM_COUNT, 0);
    }
}

GGScreen gg_screens_update(GGApp *app, const M64Input *in, GGTurnState *turn,
                           M64Board *board, GGPlayer *players,
                           M64Camera *cam, const M64Scene *scene, float dt)
{
    if (app->banner_t > 0.0f) {
        app->banner_t -= dt;
        if (app->banner_t < 0.0f) app->banner_t = 0.0f;
    }

    switch (app->screen) {
        case GG_SCREEN_TITLE:        update_title(app, in); break;
        case GG_SCREEN_CHAR_SELECT:  update_char_select(app, in); break;
        case GG_SCREEN_BOARD_SELECT:
            update_board_select(app, in, turn, board, players, cam, scene);
            break;
        case GG_SCREEN_PLAY:
            update_play(app, in, turn, board, players, cam, dt);
            break;
        case GG_SCREEN_RESULTS:
            update_results(app, in, turn, board, players, cam, scene);
            break;
    }
    return app->screen;
}

/* ── Drawing ───────────────────────────────────────────────────────────*/

/** Slow-drifting motes behind the menus.
 *
 * Twelve rectangles on lissajous paths, seeded from their own index so the
 * pattern is the same every session. It is the cheapest possible parallax:
 * the menus are flat 2D on a flat background, and without SOMETHING moving
 * behind them a title screen on this console reads as a still image with a
 * cursor on it. Drawn first, so everything else sits on top.
 */
static void draw_motes(const M64WidgetStyle *st)
{
    float t = m64_widget_time();
    for (int i = 0; i < 12; i++) {
        float ax = 0.7f + 0.5f * m64_widget_jitter(i * 3u + 1u);
        float ay = 0.5f + 0.4f * m64_widget_jitter(i * 3u + 2u);
        float ph = m64_widget_jitter(i * 3u + 5u) * 3.1416f;
        int x = (int)(SCREEN_W * 0.5f
                      + fm_sinf(t * 0.11f * ax + ph) * SCREEN_W * 0.46f);
        int y = (int)(SCREEN_H * 0.5f
                      + fm_cosf(t * 0.09f * ay + ph * 1.7f) * SCREEN_H * 0.44f);
        int sz = 2 + (i % 3);
        /* Alternating accent and border, both heavily transparent — they
         * must never compete with the text in front of them. */
        color_t c = (i & 1) ? st->accent : st->border;
        m64_gui_rect(x, y, sz, sz, RGBA32(c.r, c.g, c.b, 40 + (i % 3) * 14));
    }
}

/** A title that will not sit straight: each letter on its own bobbing
 *  baseline, drawn one character at a time.
 *
 *  m64_gui_text takes a whole string and lays it out on one baseline, so
 *  per-character motion has to be per-character CALLS. Twelve calls for a
 *  twelve-letter title is nothing next to what it buys — a static logo is
 *  the one thing on a title screen you cannot make up for elsewhere. */
static void draw_wobble_title(int cx, int y, const char *text,
                              const M64WidgetStyle *st, float amp, float rate)
{
    float t = m64_widget_time();
    int len = 0;
    while (text[len]) len++;
    int x = cx - len * M64_WIDGET_CHAR_W / 2;
    for (int i = 0; i < len; i++) {
        if (text[i] == ' ') continue;
        float ph = (float)i * 0.55f;
        int dy = (int)(amp * fm_sinf(t * rate * 6.2831853f + ph));
        int dx = (int)(amp * 0.35f * fm_cosf(t * rate * 4.4f + ph * 1.3f));
        m64_gui_text(x + i * M64_WIDGET_CHAR_W + dx, y + dy,
                     st->accent, "%c", text[i]);
    }
}

static void draw_title(const GGApp *app)
{
    const M64WidgetStyle *st = &app->style;
    draw_motes(st);

    m64_widget_panel_skew(38, 26, SCREEN_W - 76, 48, -st->lean * 1.6f,
                          st->bg, st->accent);
    draw_wobble_title(SCREEN_W / 2, 52, "GANJA GOBLIN", st, 3.2f, 0.30f);
    m64_gui_text(SCREEN_W / 2 - 11 * M64_WIDGET_CHAR_W
                 + (int)(m64_widget_jitter(99u) * 3.0f), 66, st->dim,
                 "a harvest for four goblins");

    m64_menu_draw(&app->menu, 98, 100, 124, k_title_items, NULL, st);

    m64_gui_text(SCREEN_W / 2 - 13 * M64_WIDGET_CHAR_W, SCREEN_H - 14,
                 st->dim, "D-PAD MOVE   A CONFIRM   B BACK");
}

static void draw_char_select(const GGApp *app)
{
    const M64WidgetStyle *st = &app->style;
    float t = m64_widget_time();
    uint8_t avail[GG_GOBLIN_COUNT];
    goblin_availability(app, avail);

    const char *names[GG_GOBLIN_COUNT];
    for (int g = 0; g < GG_GOBLIN_COUNT; g++) names[g] = gg_goblins[g].name;

    draw_motes(st);

    // The header bar leans the opposite way to the panels under it, so the
    // screen never resolves into a set of parallel lines.
    m64_widget_panel_skew(-4, -2, SCREEN_W + 8, 20, -st->lean,
                          st->bg, st->border);
    m64_gui_text(8, 13, st->accent, "PLAYER %d - PICK YOUR GOBLIN",
                 (int)app->picking + 1);

    // Seat colour swatch, pulsing, so it is obvious which token this pick
    // controls even from across a room.
    int pulse = (int)(2.0f * fm_sinf(t * 5.0f));
    m64_gui_rect(SCREEN_W - 22 - pulse, 3 - pulse,
                 12 + pulse * 2, 10 + pulse * 2,
                 gg_player_tint[app->picking]);

    m64_menu_draw(&app->menu, 10, 30, 122, names, avail, st);

    // Detail card for the highlighted goblin, leaning the other way and
    // bobbing on its own phase.
    const M64CharProfile *g = &gg_goblins[app->menu.cursor];
    int cx = 142 + (int)(1.8f * fm_sinf(t * 1.9f));
    int cy = 30 + (int)(1.4f * fm_cosf(t * 1.5f));
    m64_widget_panel_skew(cx, cy, SCREEN_W - cx - 8, 98, -st->lean * 1.2f,
                          st->bg, gg_player_tint[app->picking]);
    draw_wobble_title(cx + (SCREEN_W - cx - 8) / 2, cy + 16, g->name,
                      st, 1.8f, 0.55f);
    m64_gui_text(cx + 8, cy + 34, st->text, "PASSIVE");
    m64_gui_text(cx + 8, cy + 46, st->dim, "%s", g->passive_desc);
    m64_gui_text(cx + 6, cy + 64, st->text, "SPECIAL");
    m64_gui_text(cx + 6, cy + 76, st->dim, "%s", g->special_desc);
    m64_gui_text(cx + 4, cy + 92, st->warn, "charge: %d buds",
                 (int)g->charge_threshold);

    // Already-locked picks, as four crooked cards rather than a table.
    for (int p = 0; p < GG_PLAYERS; p++) {
        int bx = 12 + p * 76 + (int)(st->jitter * m64_widget_jitter(p + 40u));
        int by = 176 + (int)(st->jitter * 0.8f * m64_widget_jitter(p + 60u));
        int taken = (p < app->picking);
        color_t edge = taken ? gg_player_tint[p] : st->dim;
        m64_widget_panel_skew(bx, by, 68, 28,
                              (p & 1) ? st->lean : -st->lean, st->bg, edge);
        m64_gui_text(bx + 6, by + 12, taken ? st->text : st->dim,
                     "P%d", p + 1);
        m64_gui_text(bx + 6, by + 24, taken ? edge : st->dim,
                     "%s", taken ? gg_goblins[app->goblin[p]].name : "...");
    }
    m64_gui_text(SCREEN_W / 2 - 13 * M64_WIDGET_CHAR_W, SCREEN_H - 12,
                 st->dim, "A LOCK IN    B BACK A PLAYER");
}

static void draw_board_select(const GGApp *app)
{
    const M64WidgetStyle *st = &app->style;
    const char *names[GG_BOARD_COUNT];
    for (int b = 0; b < GG_BOARD_COUNT; b++) names[b] = gg_board_defs[b].name;

    draw_motes(st);
    float t = m64_widget_time();

    m64_widget_panel_skew(-4, -2, SCREEN_W + 8, 20, -st->lean,
                          st->bg, st->border);
    m64_gui_text(8, 13, st->accent, "PICK A BOARD");

    m64_menu_draw(&app->menu, 18, 42, 164, names, NULL, st);

    const GGBoardDef *d = &gg_board_defs[app->menu.cursor];
    int by = 116 + (int)(1.6f * fm_sinf(t * 1.3f));
    m64_widget_panel_skew(18, by, SCREEN_W - 36, 62, -st->lean * 1.3f,
                          st->bg, st->accent);
    m64_gui_text(26, by + 18, st->text, "%s", d->blurb);
    m64_gui_text(24, by + 38, st->accent, "%d rounds   %d spaces",
                 (int)d->rounds, (int)d->node_count);

    m64_gui_text(SCREEN_W / 2 - 11 * M64_WIDGET_CHAR_W, SCREEN_H - 14,
                 st->dim, "A START   B BACK TO PICKS");
}

static void draw_results(const GGApp *app, const M64Scene *scene,
                         const GGTurnState *turn, const M64Board *board,
                         const GGPlayer *players)
{
    const M64WidgetStyle *st = &app->style;

    // The board stays visible behind the table — the final token positions
    // are part of the story of the match.
    gg_hud_draw_board(scene, board, players, -1, st, SCREEN_W, SCREEN_H);

    M64PlayerSlot slots[GG_PLAYERS];
    gg_hud_fill_slots(slots, app, turn, players);

    draw_motes(st);

    int winner = app->order[0];
    char title[24];
    snprintf(title, sizeof(title), "%s WINS", slots[winner].name);

    // The winner's name goes ABOVE the panel, big and unstable, and the
    // panel's own header shrinks to "FINAL" — the first pass had both saying
    // the winner's name eighteen pixels apart, and with the wobble on top of
    // that they collided.
    (void)title;
    draw_wobble_title(SCREEN_W / 2, 26, slots[winner].name, st, 3.6f, 0.42f);
    m64_gui_text(SCREEN_W / 2 - 2 * M64_WIDGET_CHAR_W, 38, st->text, "WINS");
    m64_widget_results(46, 46, SCREEN_W - 92, "FINAL", slots, app->order,
                       GG_PLAYERS, st);

    static const char *const k_again[] = { "REMATCH", "BACK TO TITLE" };
    m64_menu_draw(&app->menu, 86, 152, 148, k_again, NULL, st);
}

void gg_screens_draw(const GGApp *app, const M64Scene *scene,
                     const GGTurnState *turn, const M64Board *board,
                     const GGPlayer *players, int screen_w, int screen_h)
{
    switch (app->screen) {
        case GG_SCREEN_TITLE:        draw_title(app); break;
        case GG_SCREEN_CHAR_SELECT:  draw_char_select(app); break;
        case GG_SCREEN_BOARD_SELECT: draw_board_select(app); break;
        case GG_SCREEN_PLAY:
            gg_hud_draw(app, scene, turn, board, players, screen_w, screen_h);
            break;
        case GG_SCREEN_RESULTS:
            draw_results(app, scene, turn, board, players);
            break;
    }

    // The fork prompt sits above whatever the screen drew, because it is
    // the only thing the player can act on while it is up.
    if (app->screen == GG_SCREEN_PLAY && turn->awaiting_branch &&
        !app->auto_play) {
        m64_widget_banner(screen_w / 2 - 80, screen_h / 2 - 12, 160, 24,
                          "<- FORK ->", 1.0f, &app->style);
    }

    // The title-screen banner ("A=PICK B=BACK") is drawn by gg_hud only on
    // the PLAY screen, so the menu screens draw theirs here.
    if (app->banner_t > 0.0f && app->screen != GG_SCREEN_PLAY) {
        float f = app->banner_total > 0.0f
                ? app->banner_t / app->banner_total : 0.0f;
        float fade = 1.0f;
        if (f > 0.8f)      fade = (1.0f - f) * 5.0f;
        else if (f < 0.2f) fade = f * 5.0f;
        m64_widget_banner(screen_w / 2 - 90, screen_h - 60, 180, 22,
                          app->banner, fade, &app->style);
    }
}
