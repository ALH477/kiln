/* SPDX-License-Identifier: MIT
 *
 * kiln_actor.c — the actor pool. See kiln_actor.h for the model.
 *
 * Two intrusive singly-linked lists share the `next` field of KilnActor: a
 * free list (`g_free_head`) and one list per category (`g_category_head`).
 * A slot is in exactly one of them at a time, so this costs nothing beyond
 * the one int16_t already in the struct.
 */

#include "kiln_actor.h"

#include <stddef.h>
#include <string.h>
#include <libdragon.h>

static const KilnActorProfile *g_profiles;
static uint16_t g_profile_count;

static KilnActor *g_pool;
static uint16_t g_pool_capacity;
static int16_t g_free_head = -1;
static int16_t g_category_head[KILN_ACTOR_CATEGORY_COUNT];

/* Deferred-despawn queue. During kiln_actor_update_all, a despawn request
 * from inside an actor's update (potentially targeting a DIFFERENT actor
 * ahead in the same category list) is deferred here and processed after
 * the full walk completes. This prevents reading `a->next` from a slot
 * already returned to the free list. */
#define KILN_DEFERRED_DESPAWN_MAX 64
static KilnActorHandle g_deferred[KILN_DEFERRED_DESPAWN_MAX];
static uint16_t g_deferred_count;
static int g_in_update;

static inline KilnActorHandle make_handle(uint16_t idx, uint16_t gen)
{
    return ((uint32_t)gen << 16) | idx;
}

void kiln_actor_system_init(const KilnActorProfile *profiles, uint16_t profile_count,
                           KilnActor *pool, uint16_t pool_capacity)
{
    assertf(pool_capacity > 0, "kiln_actor: empty pool");
    assertf(pool_capacity <= 32767, "kiln_actor: pool_capacity %u exceeds int16_t index range",
            pool_capacity);

    for (uint16_t i = 0; i < profile_count; i++) {
        assertf(profiles[i].state_size <= KILN_ACTOR_STATE_MAX,
                "kiln_actor: profile '%s' state_size %u > KILN_ACTOR_STATE_MAX %u",
                profiles[i].name ? profiles[i].name : "?",
                profiles[i].state_size, KILN_ACTOR_STATE_MAX);
    }

    g_profiles = profiles;
    g_profile_count = profile_count;
    g_pool = pool;
    g_pool_capacity = pool_capacity;

    /* Every slot's transform matrix is allocated once, here, rather than per
     * spawn/despawn — malloc_uncached churn on every actor's lifetime would
     * be both slow and a fragmentation risk. Spawn only resets the fields. */
    for (uint16_t i = 0; i < pool_capacity; i++) {
        pool[i].generation = 0;
        pool[i].next = (int16_t)((i + 1 < pool_capacity) ? (int16_t)(i + 1) : -1);
        kiln_transform_init(&pool[i].xform);
    }
    g_free_head = 0;
    g_deferred_count = 0;
    g_in_update = 0;

    for (int c = 0; c < KILN_ACTOR_CATEGORY_COUNT; c++) g_category_head[c] = -1;
}

KilnActorHandle kiln_actor_spawn(uint16_t profile_id, fm_vec3_t pos, float yaw,
                               const KilnDict *dict)
{
    assertf(profile_id < g_profile_count, "kiln_actor: bad profile_id %u", profile_id);
    if (g_free_head < 0) return KILN_ACTOR_HANDLE_NONE;

    int16_t idx = g_free_head;
    KilnActor *a = &g_pool[idx];
    g_free_head = a->next;

    const KilnActorProfile *prof = &g_profiles[profile_id];

    a->profile_id = profile_id;
    a->category = prof->category;
    a->flags = prof->default_flags;
    a->room_id = KILN_ACTOR_ROOM_NONE;
    a->health = -1;
    a->velocity = (fm_vec3_t){ { 0, 0, 0 } };
    memset(a->state, 0, KILN_ACTOR_STATE_MAX);

    a->xform.pos = pos;
    a->xform.scale = (fm_vec3_t){ { 1, 1, 1 } };
    a->xform.rot_axis = (fm_vec3_t){ { 0, 1, 0 } };
    a->xform.rot_angle = yaw;

    /* Most-recently-spawned first. O(1) here is why kiln_actor_despawn pays
     * for an O(n) unlink instead: the list has no back-pointers. */
    a->next = g_category_head[prof->category];
    g_category_head[prof->category] = idx;

    if (prof->init) prof->init(a, dict);

    return make_handle((uint16_t)idx, a->generation);
}

KilnActorHandle kiln_actor_spawn_in_room(uint16_t profile_id, fm_vec3_t pos, float yaw,
                                       uint8_t room_id, const KilnDict *dict)
{
    KilnActorHandle h = kiln_actor_spawn(profile_id, pos, yaw, dict);
    if (h == KILN_ACTOR_HANDLE_NONE) return h;
    KilnActor *a = kiln_actor_resolve(h);
    /* kiln_actor_spawn never returns NONE alongside a valid handle, but assert
     * the resolve anyway — a stale handle would silently despawn the wrong
     * room's actors. */
    assertf(a != NULL, "kiln_actor: spawn returned a handle that does not resolve");
    a->room_id = room_id;
    return h;
}

static void despawn_immediate(KilnActorHandle h)
{
    KilnActor *a = kiln_actor_resolve(h);
    if (!a) return;
    uint16_t idx = (uint16_t)(h & 0xFFFFu);

    const KilnActorProfile *prof = &g_profiles[a->profile_id];
    if (prof->destroy) prof->destroy(a);

    int16_t *link = &g_category_head[a->category];
    while (*link != -1 && *link != (int16_t)idx) link = &g_pool[*link].next;
    if (*link == (int16_t)idx) *link = a->next;

    a->generation++;
    a->next = g_free_head;
    g_free_head = (int16_t)idx;
}

void kiln_actor_despawn(KilnActorHandle h)
{
    if (h == KILN_ACTOR_HANDLE_NONE) return;

    /* If we're inside kiln_actor_update_all, defer the actual despawn so
     * that a despawn targeting an actor ahead in the same list doesn't
     * corrupt the walk. kiln_actor_update_all processes the queue after
     * the walk completes. Self-despawn (the common case) also goes through
     * the queue for simplicity — it's one extra function call, and the
     * next_idx capture already handles it. */
    if (g_in_update) {
        if (g_deferred_count < KILN_DEFERRED_DESPAWN_MAX) {
            g_deferred[g_deferred_count++] = h;
        } else {
            debugf("kiln_actor: deferred despawn queue full, dropping handle\n");
        }
        return;
    }

    despawn_immediate(h);
}

KilnActor *kiln_actor_resolve(KilnActorHandle h)
{
    if (h == KILN_ACTOR_HANDLE_NONE) return NULL;
    uint16_t idx = (uint16_t)(h & 0xFFFFu);
    uint16_t gen = (uint16_t)(h >> 16);
    if (idx >= g_pool_capacity) return NULL;
    KilnActor *a = &g_pool[idx];
    return (a->generation == gen) ? a : NULL;
}

KilnActorHandle kiln_actor_handle_of(const KilnActor *a)
{
    ptrdiff_t idx = a - g_pool;
    assertf(idx >= 0 && idx < g_pool_capacity,
            "kiln_actor: handle_of called on a pointer not in this pool");
    return make_handle((uint16_t)idx, a->generation);
}

void kiln_actor_update_all(float dt)
{
    g_in_update = 1;
    for (int c = 0; c < KILN_ACTOR_CATEGORY_COUNT; c++) {
        int16_t idx = g_category_head[c];
        while (idx != -1) {
            KilnActor *a = &g_pool[idx];
            /* Captured before calling update: if the actor despawns itself
             * (or is despawned by another actor's update), the slot may
             * already be back on the free list by the time we read next.
             * Deferring despawns to after the walk fixes this — the slot
             * stays on the category list (with its next intact) until the
             * deferred queue is drained below. */
            int16_t next_idx = a->next;
            if (!(a->flags & KILN_ACTOR_FLAG_PAUSED)) {
                const KilnActorProfile *prof = &g_profiles[a->profile_id];
                if (prof->update) prof->update(a, dt);
            }
            idx = next_idx;
        }
    }
    g_in_update = 0;

    /* Process deferred despawns. A despawn here may post another despawn
     * (e.g. a destroy callback despawns a child), but g_in_update is 0 so
     * those go through despawn_immediate directly. */
    for (uint16_t i = 0; i < g_deferred_count; i++) {
        despawn_immediate(g_deferred[i]);
    }
    g_deferred_count = 0;
}

void kiln_actor_draw_all(void)
{
    for (int c = 0; c < KILN_ACTOR_CATEGORY_COUNT; c++) {
        for (int16_t idx = g_category_head[c]; idx != -1; idx = g_pool[idx].next) {
            KilnActor *a = &g_pool[idx];
            if (a->flags & KILN_ACTOR_FLAG_NO_DRAW) continue;
            const KilnActorProfile *prof = &g_profiles[a->profile_id];
            if (!prof->draw) continue;
            /* The system applies the actor's world transform so every draw
             * callback starts from the same place OoT's does: geometry drawn
             * in the actor's own local space. */
            kiln_transform_push(&a->xform);
            prof->draw(a);
            kiln_transform_pop();
        }
    }
}

KilnActor *kiln_actor_first(uint8_t category)
{
    if (category >= KILN_ACTOR_CATEGORY_COUNT) return NULL;
    int16_t idx = g_category_head[category];
    return idx == -1 ? NULL : &g_pool[idx];
}

KilnActor *kiln_actor_next(KilnActor *cur)
{
    if (!cur) return NULL;
    int16_t idx = cur->next;
    return idx == -1 ? NULL : &g_pool[idx];
}

uint16_t kiln_actor_count(uint8_t category)
{
    uint16_t n = 0;
    if (category > KILN_ACTOR_CATEGORY_COUNT) return 0;
    int c0 = (category == KILN_ACTOR_CATEGORY_COUNT) ? 0 : category;
    int c1 = (category == KILN_ACTOR_CATEGORY_COUNT) ? KILN_ACTOR_CATEGORY_COUNT - 1 : category;
    for (int c = c0; c <= c1; c++)
        for (int16_t idx = g_category_head[c]; idx != -1; idx = g_pool[idx].next) n++;
    return n;
}

void kiln_actor_dispatch_event(KilnActor *a, uint16_t event_id,
                              const int32_t *args, uint8_t argc)
{
    assertf(a != NULL, "kiln_actor: dispatch_event on NULL");
    const KilnActorProfile *prof = &g_profiles[a->profile_id];
    if (!prof->event) return;
    prof->event(a, event_id, args, argc);
}