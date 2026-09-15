/* SPDX-License-Identifier: MIT
 *
 * kiln_context's A-button scan, asserted on the host with the real
 * kiln_context.c and kiln_actor.c.
 *
 * The door branch compared the loop index into its category table against the
 * DOOR category's enum value, so it never ran: a locked door said "Open" and
 * the UNLOCK action could not be produced at all. Nothing noticed, because the
 * one game using it checks the key itself. So the contract is pinned here: a
 * door with health > 0 (its key id) is UNLOCK, health 0 is OPEN, a chest is
 * OPEN_CHEST, an NPC is TALK, and something behind the player is nothing.
 */
#include <kiln_context.h>
#include <kiln_actor.h>
#include <libdragon.h>

#include <stdio.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

enum { P_NPC, P_DOOR, P_CHEST, P_COUNT };
static const KilnActorProfile PROFILES[P_COUNT] = {
    [P_NPC]   = { .name = "npc",   .category = KILN_ACTOR_CAT_NPC },
    [P_DOOR]  = { .name = "door",  .category = KILN_ACTOR_CAT_DOOR },
    [P_CHEST] = { .name = "chest", .category = KILN_ACTOR_CAT_CHEST },
};
static KilnActor g_pool[8];

static KilnContextAction scan_ahead(void)
{
    KilnActorHandle h;
    return kiln_context_scan((fm_vec3_t){{ 0, 0, 0 }}, 0.0f /* facing +Z */, 40.0f, 0.5f, &h);
}

int main(void)
{
    kiln_actor_system_init(PROFILES, P_COUNT, g_pool, 8);

    KilnActorHandle door = kiln_actor_spawn(P_DOOR, (fm_vec3_t){{ 0, 0, 20 }}, 0, NULL);
    kiln_actor_resolve(door)->health = 1;
    KilnContextAction a = scan_ahead();
    CHECK(a == KILN_CTX_UNLOCK, "a locked door (health 1) scanned as %d (%s), expected UNLOCK",
          a, kiln_context_label(a));
    kiln_actor_resolve(door)->health = 0;
    a = scan_ahead();
    CHECK(a == KILN_CTX_OPEN, "an unlocked door (health 0) scanned as %d (%s), expected OPEN",
          a, kiln_context_label(a));
    kiln_actor_despawn(door);

    KilnActorHandle chest = kiln_actor_spawn(P_CHEST, (fm_vec3_t){{ 0, 0, 20 }}, 0, NULL);
    a = scan_ahead();
    CHECK(a == KILN_CTX_OPEN_CHEST, "a chest scanned as %d, expected OPEN_CHEST", a);
    kiln_actor_despawn(chest);

    KilnActorHandle npc = kiln_actor_spawn(P_NPC, (fm_vec3_t){{ 0, 0, 20 }}, 0, NULL);
    a = scan_ahead();
    CHECK(a == KILN_CTX_TALK, "an NPC scanned as %d, expected TALK", a);
    kiln_actor_resolve(npc)->xform.pos = (fm_vec3_t){{ 0, 0, -20 }};
    a = scan_ahead();
    CHECK(a == KILN_CTX_NONE, "an NPC behind the player scanned as %d, expected NONE", a);

    if (fails) { printf("\nFAILED (%d)\n", fails); return 1; }
    printf("kiln_context: doors, chests and NPCs map to their actions\n");
    return 0;
}
