// SPDX-License-Identifier: MPL-2.0

#include "gg_hud.h"
#include "gg_goblins.h"
#include "gg_buds.h"

#include <kiln/kiln_widget.h>

// Space-type colours. The design doc asks for "limited but vibrant colour
// palettes", so this is six hues that stay distinguishable at 320x240 on a
// CRT rather than a smooth ramp: green grows, brown dries, gold trades,
// violet is spirit, pink is a mini-game, cyan is a shortcut.
static color_t space_color(KilnSpaceType t)
{
    switch (t) {
        case KILN_SPACE_START:    return RGBA32(220, 220, 235, 255);
        case KILN_SPACE_GROW:     return RGBA32(  0, 220, 100, 255);
        case KILN_SPACE_DRY:      return RGBA32(170, 110,  60, 255);
        case KILN_SPACE_TRADE:    return RGBA32(255, 200,  80, 255);
        case KILN_SPACE_SPIRIT:   return RGBA32(180, 120, 255, 255);
        case KILN_SPACE_MINIGAME: return RGBA32(255, 110, 190, 255);
        case KILN_SPACE_SHORTCUT: return RGBA32( 90, 220, 255, 255);
        default:                 return RGBA32(128, 128, 128, 255);
    }
}

static const char *status_note(GGStatus s)
{
    switch (s) {
        case GG_STATUS_COUCH_LOCK: return "COUCH";
        case GG_STATUS_MUNCHIES:   return "MUNCH";
        case GG_STATUS_PARANOIA:   return "PARA";
        default:                   return NULL;
    }
}

void gg_hud_draw_board(const KilnScene *scene, const KilnBoard *board,
                       const GGPlayer *players, int active_player,
                       const KilnWidgetStyle *st, int screen_w, int screen_h)
{
    (void)st;

    // Project every node once, up front. The edge pass needs both
    // endpoints' screen positions and the token pass needs the node a
    // player is standing on, so projecting per-use would redo the same
    // maths three times for a 12-node board.
    int sx[KILN_BOARD_MAX_NODES], sy[KILN_BOARD_MAX_NODES];
    uint8_t vis[KILN_BOARD_MAX_NODES];
    int n = board->node_count;
    if (n > KILN_BOARD_MAX_NODES) n = KILN_BOARD_MAX_NODES;

    for (int i = 0; i < n; i++) {
        vis[i] = (uint8_t)kiln_scene_project(scene, board->nodes[i].pos,
                                            screen_w, screen_h, &sx[i], &sy[i]);
    }

    // Edges first, so nodes draw on top of the path rather than under it.
    // kiln_gui has no line primitive (a HUD needs rectangles, and a general
    // line rasteriser is a lot of engine for one caller), so each edge is
    // a run of small dots stepped in SCREEN space — which also means a
    // near-vertical edge and a near-horizontal one cost the same.
    for (int i = 0; i < n; i++) {
        if (!vis[i]) continue;
        const KilnBoardNode *nd = &board->nodes[i];
        for (int e = 0; e < nd->next_count; e++) {
            int j = nd->next[e];
            if (j < 0 || j >= n || !vis[j]) continue;
            int dx = sx[j] - sx[i], dy = sy[j] - sy[i];
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            int span = adx > ady ? adx : ady;
            int dots = span / 7;
            if (dots > 12) dots = 12;   // bound the work on a long edge
            // A fork's second branch draws dimmer, so a player can see at a
            // glance which way the default step goes.
            color_t c = (e == 0) ? RGBA32(90, 90, 120, 255)
                                 : RGBA32(70, 110, 130, 255);
            for (int k = 1; k < dots; k++) {
                int px = sx[i] + dx * k / dots;
                int py = sy[i] + dy * k / dots;
                kiln_gui_rect(px - 1, py - 1, 2, 2, c);
            }
        }
    }

    // Nodes.
    for (int i = 0; i < n; i++) {
        if (!vis[i]) continue;
        color_t c = space_color(board->nodes[i].type);
        kiln_gui_panel(sx[i] - 5, sy[i] - 5, 10, 10, c,
                      RGBA32(20, 20, 30, 255));
    }

    // Tokens last, on top of everything. Four players sharing a node would
    // draw as one marker, so each seat gets a fixed screen-space offset in
    // a 2x2 arrangement around the node — a stable position per seat reads
    // better than a stacking order that shuffles when someone moves.
    static const int off_x[GG_PLAYERS] = { -7,  7, -7,  7 };
    static const int off_y[GG_PLAYERS] = { -7, -7,  7,  7 };
    for (int p = 0; p < GG_PLAYERS; p++) {
        int nd = players[p].node;
        if (nd < 0 || nd >= n || !vis[nd]) continue;
        int px = sx[nd] + off_x[p];
        int py = sy[nd] + off_y[p];
        int size = (p == active_player) ? 8 : 6;
        kiln_gui_panel(px - size / 2, py - size / 2, size, size,
                      gg_player_tint[p],
                      p == active_player ? RGBA32(255, 255, 255, 255)
                                         : RGBA32(20, 20, 30, 255));
    }
}

void gg_hud_fill_slots(KilnPlayerSlot *slots, const GGApp *app,
                       const GGTurnState *turn, const GGPlayer *players)
{
    for (int p = 0; p < GG_PLAYERS; p++) {
        const KilnCharState *ch = &turn->chars[p];
        float charge = -1.0f;
        int ready = 0;
        if (ch->active && ch->profile->charge_threshold > 0) {
            charge = (float)ch->charge / (float)ch->profile->charge_threshold;
            if (charge > 1.0f) charge = 1.0f;
            ready = (charge >= 1.0f && ch->cooldown == 0);
        }
        slots[p] = (KilnPlayerSlot){
            .name   = ch->active ? ch->profile->name : "OPEN",
            .note   = status_note(players[p].status),
            .score  = players[p].buds,
            .charge = charge,
            .tint   = gg_player_tint[p],
            .active = (uint8_t)(app->screen == GG_SCREEN_PLAY &&
                                p == turn->turn.player),
            .ready  = (uint8_t)ready,
        };
    }
}

static const char *phase_name(KilnTurnPhase p)
{
    switch (p) {
        case KILN_PHASE_ROLL:  return "ROLL";
        case KILN_PHASE_MOVE:  return "MOVE";
        case KILN_PHASE_LAND:  return "LAND";
        case KILN_PHASE_EVENT: return "EVENT";
        case KILN_PHASE_END:   return "END";
        default:              return "?";
    }
}

void gg_hud_draw(const GGApp *app, const KilnScene *scene,
                 const GGTurnState *turn, const KilnBoard *board,
                 const GGPlayer *players, int screen_w, int screen_h)
{
    const KilnWidgetStyle *st = &app->style;

    gg_hud_draw_board(scene, board, players, turn->turn.player,
                      st, screen_w, screen_h);

    // Top bar: board name, round, phase.
    kiln_gui_panel(0, 0, screen_w, 16, st->bg, st->border);
    kiln_gui_text(4, 12, st->accent, "%s", gg_board_defs[app->board].name);
    kiln_gui_text(screen_w - 15 * KILN_WIDGET_CHAR_W, 12, st->text,
                 "R%2d/%-2d %-5s", (int)turn->turn.round, (int)app->rounds,
                 phase_name(turn->turn.phase));

    // Player strip, top-left under the bar.
    KilnPlayerSlot slots[GG_PLAYERS];
    gg_hud_fill_slots(slots, app, turn, players);
    kiln_widget_hud_strip(4, 20, 150, slots, GG_PLAYERS, st);

    // Dice, top-right. It spins during ROLL (the phase where the result is
    // being decided) and holds the result from MOVE onward.
    int rolling = (turn->turn.phase == KILN_PHASE_ROLL);
    kiln_widget_dice(screen_w - 40, 22, 32, turn->last_roll, rolling,
                    turn->roll_anim_t, st);

    // Log, bottom strip. Six lines is what fits without covering the board.
    int log_h = 6 * 10 + 6;
    int log_y = screen_h - log_h;
    kiln_gui_panel(0, log_y, screen_w, log_h, st->bg, st->border);
    for (int i = 0; i < 6; i++) {
        int idx = (turn->log_head + i) % 6;
        if (!turn->log[idx][0]) continue;
        // The newest entry is the one just before log_head; brighten it so
        // the eye lands on what changed this frame.
        int newest = ((turn->log_head + 5) % 6) == idx;
        kiln_gui_text(4, log_y + 12 + i * 10,
                     newest ? st->accent : st->dim, "%s", turn->log[idx]);
    }

    if (app->banner_t > 0.0f) {
        // Fade in over the first 20% and out over the last 20%; hold in
        // between, so a short banner still reads as deliberate.
        float f = app->banner_total > 0.0f
                ? app->banner_t / app->banner_total : 0.0f;
        float fade = 1.0f;
        if (f > 0.8f)      fade = (1.0f - f) * 5.0f;
        else if (f < 0.2f) fade = f * 5.0f;
        // Below the player strip (which ends at y=108), not across it.
        kiln_widget_banner(screen_w / 2 - 92, 118, 184, 26,
                          app->banner, fade, st);
    }
}
