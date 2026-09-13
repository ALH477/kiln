/* SPDX-License-Identifier: MIT
 *
 * kiln_room's streaming set, asserted on the host with the real kiln_room.c,
 * kiln_actor.c and kiln_clip.c.
 *
 * The property is that the loaded set is a function of where the camera is:
 * the room it is in, anything its box touches, and those rooms' neighbours,
 * capped at max_loaded. For as long as rooms-demo existed that was not true —
 * a loaded room stayed loaded and the set grew a ring of neighbours a frame
 * until append_loaded asserted — and nothing could show it, because the demo's
 * player could not leave room A. So this walks a camera across a 2x2 grid and
 * checks the set on every frame of the walk, not just at the ends.
 */
#include <kiln_room.h>
#include <kiln_actor.h>
#include <kiln_clip.h>
#include <libdragon.h>

#include <stdio.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int g_loads[4], g_unloads[4];

static void on_load(KilnRoom *r, void *u)   { (void)u; g_loads[r->id]++; }
static void on_unload(KilnRoom *r, void *u) { (void)u; g_unloads[r->id]++; r->brushes = NULL; r->brush_count = 0; }
static void on_spawn(KilnRoom *r, const KilnRoomSpawn *s, void *u) { (void)r; (void)s; (void)u; }

/* A B      each 160 x 160, y 0..60
 * C D      neighbours are edge-adjacent rooms only */
static KilnRoom g_rooms[4] = {
    { .id = 0, .aabb_min = {{   0, 0,   0 }}, .aabb_max = {{ 160, 60, 160 }}, .neighbour_count = 2, .neighbours = { 1, 2 } },
    { .id = 1, .aabb_min = {{ 160, 0,   0 }}, .aabb_max = {{ 320, 60, 160 }}, .neighbour_count = 2, .neighbours = { 0, 3 } },
    { .id = 2, .aabb_min = {{   0, 0, 160 }}, .aabb_max = {{ 160, 60, 320 }}, .neighbour_count = 2, .neighbours = { 0, 3 } },
    { .id = 3, .aabb_min = {{ 160, 0, 160 }}, .aabb_max = {{ 320, 60, 320 }}, .neighbour_count = 2, .neighbours = { 1, 2 } },
};

static const KilnActorProfile PROFILES[1] = { { .name = "prop", .category = KILN_ACTOR_CAT_PROP } };
static KilnActor g_pool[8];

static int loaded(KilnRoomSystem *s, int id) { return kiln_room_loaded(s, (uint8_t)id) != NULL; }

static void step(KilnRoomSystem *s, float x, float z)
{
    kiln_room_system_update(s, (fm_vec3_t){{ x, 10, z }});
}

int main(void)
{
    kiln_actor_system_init(PROFILES, 1, g_pool, 8);
    KilnRoomSystem sys;
    kiln_room_system_init(&sys, g_rooms, 4, 3, on_load, on_unload, on_spawn, NULL, NULL, 1);

    for (int f = 0; f < 5; f++) step(&sys, 80, 80);
    CHECK(kiln_room_loaded_count(&sys) == 3 && loaded(&sys, 0) && loaded(&sys, 1) && loaded(&sys, 2),
          "in the middle of A: expected A, B and C loaded (count %u, D %d)",
          kiln_room_loaded_count(&sys), loaded(&sys, 3));
    CHECK(!loaded(&sys, 3), "in the middle of A, the diagonal room D is loaded");
    CHECK(kiln_room_current(&sys) && kiln_room_current(&sys)->id == 0, "current room is not A");

    /* Walk A -> B -> D, checking the set every frame. */
    int worst = 0;
    for (float x = 80; x <= 240; x += 2) {
        step(&sys, x, 80);
        if (kiln_room_loaded_count(&sys) > worst) worst = kiln_room_loaded_count(&sys);
    }
    for (float z = 80; z <= 240; z += 2) {
        step(&sys, 240, z);
        if (kiln_room_loaded_count(&sys) > worst) worst = kiln_room_loaded_count(&sys);
    }
    CHECK(worst <= 3, "the loaded set reached %d rooms; max_loaded is 3", worst);
    CHECK(loaded(&sys, 3) && loaded(&sys, 1) && loaded(&sys, 2) && !loaded(&sys, 0),
          "in the middle of D: expected B, C, D loaded and A not (A %d B %d C %d D %d)",
          loaded(&sys, 0), loaded(&sys, 1), loaded(&sys, 2), loaded(&sys, 3));
    CHECK(g_unloads[0] >= 1, "room A was never unloaded on the way to D");
    CHECK(kiln_room_current(&sys) && kiln_room_current(&sys)->id == 3, "current room is not D");

    /* Straddling the A|B border, both rooms the camera touches are in. */
    for (int f = 0; f < 3; f++) step(&sys, 160, 80);
    CHECK(loaded(&sys, 0) && loaded(&sys, 1), "on the A|B border, A %d B %d", loaded(&sys, 0), loaded(&sys, 1));
    CHECK(kiln_room_loaded_count(&sys) <= 3, "on the border the set is %u rooms", kiln_room_loaded_count(&sys));

    /* Nowhere near any room: nothing is wanted, so nothing stays. */
    step(&sys, 2000, 2000);
    CHECK(kiln_room_loaded_count(&sys) == 0, "far from every room, %u stay loaded",
          kiln_room_loaded_count(&sys));
    CHECK(kiln_room_current(&sys) == NULL, "far from every room there is still a current room");

    printf("  loads   A %d B %d C %d D %d\n  unloads A %d B %d C %d D %d\n",
           g_loads[0], g_loads[1], g_loads[2], g_loads[3],
           g_unloads[0], g_unloads[1], g_unloads[2], g_unloads[3]);
    if (fails) { printf("\nFAILED (%d)\n", fails); return 1; }
    printf("kiln_room: the loaded set follows the camera and never exceeds max_loaded\n");
    return 0;
}
