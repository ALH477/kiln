/* SPDX-License-Identifier: MIT
 *
 * kiln_context.h — context-sensitive action button. OoT's A-button: the
 * same button does different things depending on what the player is
 * standing near and facing.
 *
 * ── How it works ───────────────────────────────────────────────────────
 * Each frame, kiln_context_scan walks the NPC, DOOR, CHEST, PROP, and ITEM
 * category lists and finds the closest actor within a forward-facing arc
 * (~2 m, ~60° cone). It returns a KilnContextAction describing what A would
 * do right now. The HUD shows the label ("Talk", "Open", "Unlock").
 * When the player presses A, kiln_context_execute dispatches the action.
 *
 * ── Why a separate module, not inline in main.c ───────────────────────
 * The scan logic (cone test, category walk, distance check) is generic
 * and reusable. The execution dispatch (which event to post, which key
 * to check) is game-specific. The module handles the scan; the game
 * handles the execution, so the scan doesn't need to know about
 * inventories or key ids.
 *
 * ── The OoT A-button philosophy ────────────────────────────────────────
 * OoT's A button is never unmapped — it always does SOMETHING. Near an
 * NPC: "Speak". Near a door: "Open". Near a chest: "Open". Near an
 * item: "Grab". In open space: nothing (or jump in a platformer). The
 * context system is what makes that possible: it tells the game what
 * A should do THIS frame, and the game routes the input accordingly.
 */
#ifndef KILN_CONTEXT_H
#define KILN_CONTEXT_H

#include <t3d/t3dmath.h>
#include <stdint.h>
#include "kiln_actor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KILN_CTX_NONE = 0,
    KILN_CTX_TALK,
    KILN_CTX_OPEN,
    KILN_CTX_UNLOCK,
    KILN_CTX_OPEN_CHEST,
    KILN_CTX_USE,
    KILN_CTX_PICKUP,
} KilnContextAction;

/** Scan for the nearest interactable actor within `max_dist` and a
 *  forward-facing cone of half-angle `cone_half` (radians). Returns the
 *  action and sets `out_actor` to the handle. The action type is derived
 *  from the actor's category: NPC→TALK, DOOR→OPEN (or UNLOCK if the
 *  actor's flags indicate locked), CHEST→OPEN_CHEST, PROP→USE,
 *  ITEM→PICKUP. Returns KILN_CTX_NONE if nothing is in range. */
KilnContextAction kiln_context_scan(fm_vec3_t player_pos, float yaw,
                                   float max_dist, float cone_half,
                                   KilnActorHandle *out_actor);

/** Human-readable label for the action, for HUD display. */
const char *kiln_context_label(KilnContextAction action);

#ifdef __cplusplus
}
#endif

#endif /* KILN_CONTEXT_H */