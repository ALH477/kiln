// SPDX-License-Identifier: MPL-2.0

#include "gg_turn.h"
#include "gg_spaces.h"
#include "gg_buds.h"

#include <stdarg.h>
#include <stdio.h>

void gg_turn_init(GGTurnState *s, uint16_t max_rounds)
{
    kiln_dice_init_uniform(&s->die, 6);
    kiln_rng_seed(&s->rng, 0xC0FFEE);
    gg_turn_restart(s, max_rounds);
}

void gg_turn_restart(GGTurnState *s, uint16_t max_rounds)
{
    kiln_turn_init(&s->turn, GG_PLAYERS, max_rounds);
    s->last_roll = 0;
    s->last_player = 0;
    s->roll_anim_t = 0.0f;
    s->steps_left = 0;
    s->branch_choice = -1;
    s->awaiting_branch = 0;
    s->log_head = 0;
    for (int i = 0; i < 6; i++) s->log[i][0] = '\0';
    for (int i = 0; i < GG_PLAYERS; i++) kiln_char_init(&s->chars[i]);
}

void gg_turn_log(GGTurnState *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->log[s->log_head], sizeof(s->log[0]), fmt, ap);
    va_end(ap);
    s->log_head = (s->log_head + 1) % 6;
}

void gg_turn_step(GGTurnState *s, const KilnBoard *board, GGPlayer *players)
{
    int cur = s->turn.player;
    GGPlayer *p = &players[cur];
    KilnCharState *ch = &s->chars[cur];

    // Couch Lock: skip the whole turn.
    if (s->turn.phase == KILN_PHASE_ROLL &&
        p->status == GG_STATUS_COUCH_LOCK && p->status_turns > 0) {
        gg_turn_log(s, "P%d COUCH LOCKED -> skip", cur + 1);
        p->status_turns--;
        if (p->status_turns == 0) p->status = GG_STATUS_NONE;
        kiln_turn_skip_to_end(&s->turn);
        // Still tick the cooldown + turns_played at END.
        kiln_char_tick_cooldown(ch);
        p->turns_played++;
        kiln_turn_advance_phase(&s->turn);
        return;
    }

    switch (s->turn.phase) {
        case KILN_PHASE_ROLL: {
            // Clear per-turn passive bonuses before ticking the passive.
            p->pending_move_bonus = 0;
            p->pending_bud_bonus = 0;

            kiln_char_tick_passive(ch, p, 0.0f);

            s->last_roll = kiln_dice_roll(&s->die, &s->rng);
            s->last_player = cur;

            // Apply Glimmer's "behind" +1 to the roll (passive set the
            // pending_move_bonus flag).
            if (p->pending_move_bonus) {
                s->last_roll += 1;
                p->pending_move_bonus = 0;
            }
            // Clamp to a sane max so a 6+bonus doesn't walk off the board.
            if (s->last_roll > 6) s->last_roll = 6;

            gg_turn_log(s, "P%d rolled %d", cur + 1, s->last_roll);
            // Sparky's extra movement is a movement bonus, not a die
            // result — it is added to the walk here rather than to
            // last_roll so the dice widget still shows the face that was
            // actually rolled.
            s->steps_left = s->last_roll + p->pending_move_bonus;
            p->pending_move_bonus = 0;
            s->branch_choice = -1;
            s->awaiting_branch = 0;
            s->roll_anim_t = 0.0f;
            kiln_turn_advance_phase(&s->turn);
            break;
        }
        case KILN_PHASE_MOVE: {
            if (s->steps_left <= 0) {
                s->awaiting_branch = 0;
                kiln_turn_advance_phase(&s->turn);
                break;
            }
            const KilnBoardNode *nd = &board->nodes[p->node];
            if (nd->next_count > 1 && s->branch_choice < 0) {
                // At a fork with no choice made: hold here. The caller
                // (gg_screens) shows the prompt and writes branch_choice
                // when the player picks. Deliberately no timeout — a party
                // game that moves your token for you while you are deciding
                // is worse than one that waits.
                s->awaiting_branch = 1;
                break;
            }
            uint8_t br = (uint8_t)(s->branch_choice > 0 ? s->branch_choice : 0);
            if (br >= nd->next_count) br = 0;
            p->node = kiln_board_step(board, p->node, br);
            s->steps_left--;
            s->branch_choice = -1;
            s->awaiting_branch = 0;
            if (s->steps_left <= 0) kiln_turn_advance_phase(&s->turn);
            break;
        }
        case KILN_PHASE_LAND: {
            KilnSpaceType st = board->nodes[p->node].type;
            int award = gg_space_enter(st, p);
            // Dank's passive: +1 bud per Grow space (pending_bud_bonus=1).
            if (st == KILN_SPACE_GROW && p->pending_bud_bonus > 0) {
                award += p->pending_bud_bonus;
                p->pending_bud_bonus = 0;
            }
            gg_buds_add(p, award);
            // Charge accrues per bud collected, per the spec.
            if (award > 0) kiln_char_add_charge(ch, (uint16_t)award);

            // Auto-fire the special when it's ready. Phase 4 will gate
            // this on a button press; for now, fire-and-log so the
            // state machine is observable.
            if (ch->active && ch->charge >= ch->profile->charge_threshold &&
                ch->cooldown == 0) {
                // Target: the player with the most buds (rival).
                int target = -1;
                int max_buds = -1;
                for (int i = 0; i < GG_PLAYERS; i++) {
                    if (i == cur) continue;
                    if (players[i].buds > max_buds) {
                        max_buds = players[i].buds;
                        target = i;
                    }
                }
                if (target >= 0) {
                    int fired = kiln_char_try_special(ch, p, &players[target]);
                    if (fired) {
                        gg_turn_log(s, "P%d SPECIAL: %s",
                                    cur + 1, ch->profile->name);
                    }
                }
            }

            gg_turn_log(s, "P%d %s %+d -> %d", cur + 1,
                        st == KILN_SPACE_GROW     ? "GROW"  :
                        st == KILN_SPACE_DRY      ? "DRY "  :
                        st == KILN_SPACE_SPIRIT   ? "SPRT"  :
                        st == KILN_SPACE_MINIGAME ? "GAME"  :
                        st == KILN_SPACE_START    ? "STRT"  : "????",
                        award, p->buds);
            kiln_turn_advance_phase(&s->turn);
            break;
        }
        case KILN_PHASE_EVENT:
            // Phase 5: Harvest Event / Party Mode hooks land here.
            kiln_turn_advance_phase(&s->turn);
            break;
        case KILN_PHASE_END:
            kiln_char_tick_cooldown(ch);
            p->turns_played++;
            // Tick status expiry.
            if (p->status != GG_STATUS_NONE) {
                if (p->status_turns > 0) p->status_turns--;
                if (p->status_turns == 0) p->status = GG_STATUS_NONE;
            }
            kiln_turn_advance_phase(&s->turn);
            break;
        default:
            kiln_turn_advance_phase(&s->turn);
            break;
    }
}