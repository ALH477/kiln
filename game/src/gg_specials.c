// SPDX-License-Identifier: MPL-2.0

#include "gg_specials.h"
#include "gg_types.h"
#include "gg_buds.h"

#include <string.h>

// The specials need to reach across players (Couch Lock targets a rival,
// Lucky Puff steals from everyone). The engine's hook signature passes
// only the active player as `game_player`; this file keeps a pointer to
// the whole player array, set once at boot by the game side.
static GGPlayer *g_players = 0;
static int       g_player_count = 0;

void gg_specials_set_players(GGPlayer *players, int count)
{
    g_players = players;
    g_player_count = count;
}

// ── Passives ────────────────────────────────────────────────────────────

void gg_passive_dank(KilnCharState *self, void *game_player, float dt)
{
    (void)self; (void)dt;
    GGPlayer *p = (GGPlayer *)game_player;
    // Accrue during ROLL; consumed during LAND on Grow spaces via
    // gg_turn's award calculation.
    p->pending_bud_bonus = 1;
}

void gg_passive_sparky(KilnCharState *self, void *game_player, float dt)
{
    (void)self; (void)dt;
    GGPlayer *p = (GGPlayer *)game_player;
    // +1 movement every 3rd turn (turns_played 2, 5, 8, ...). The bonus is
    // consumed during MOVE.
    if (p->turns_played > 0 && (p->turns_played % 3) == 0) {
        p->pending_move_bonus = 1;
    }
}

void gg_passive_moss(KilnCharState *self, void *game_player, float dt)
{
    (void)self; (void)dt; (void)game_player;
    // Phase 5 will extend item durations by 1 when this player holds an
    // item. Nothing to do yet; the field is here so the passive is wired
    // and the HUD can advertise it.
}

void gg_passive_glimmer(KilnCharState *self, void *game_player, float dt)
{
    (void)self; (void)dt;
    GGPlayer *p = (GGPlayer *)game_player;
    if (!g_players || g_player_count <= 0) return;
    // "Behind" = not in the lead. Find the max bud count; if the active
    // player is below it, bump the roll by 1. The bonus is consumed in
    // ROLL via gg_turn.
    int max_buds = 0;
    for (int i = 0; i < g_player_count; i++) {
        if (g_players[i].buds > max_buds) max_buds = g_players[i].buds;
    }
    if (p->buds < max_buds) {
        p->pending_move_bonus = 1;  // reuses the +1 field; cleared in ROLL
    }
}

// ── Specials ────────────────────────────────────────────────────────────

int gg_special_dank(KilnCharState *self, void *game_player, void *target)
{
    (void)self;
    GGPlayer *p = (GGPlayer *)game_player;
    GGPlayer *rival = (GGPlayer *)target;
    if (!rival) return 0;
    // Couch Lock: the target loses their next turn (status for 1 turn).
    rival->status = GG_STATUS_COUCH_LOCK;
    rival->status_turns = 1;
    (void)p;
    return 1;
}

int gg_special_sparky(KilnCharState *self, void *game_player, void *target)
{
    (void)self; (void)target;
    GGPlayer *p = (GGPlayer *)game_player;
    // Spark Plug: force a mini-game with bonus rewards. Mini-games are
    // deferred to Phase 8; for now award a flat bonus so the special is
    // observable.
    gg_buds_add(p, 5);
    return 1;
}

int gg_special_moss(KilnCharState *self, void *game_player, void *target)
{
    (void)self; (void)target;
    GGPlayer *p = (GGPlayer *)game_player;
    // Resin Trap: leave a sticky hazard. The hazard system lands with
    // Phase 5's item table; for now just flag a status on the active
    // player so the HUD can advertise the trap was placed.
    p->status = GG_STATUS_PARANOIA;  // reused as "trap placed" marker
    p->status_turns = 2;
    return 1;
}

int gg_special_glimmer(KilnCharState *self, void *game_player, void *target)
{
    (void)self; (void)target;
    GGPlayer *p = (GGPlayer *)game_player;
    if (!g_players) return 0;
    // Lucky Puff: steal 1 bud from every other player.
    int stolen = 0;
    for (int i = 0; i < g_player_count; i++) {
        if (&g_players[i] == p) continue;
        if (g_players[i].buds > 0) {
            g_players[i].buds--;
            stolen++;
        }
    }
    gg_buds_add(p, stolen);
    return stolen > 0 ? 1 : 0;
}