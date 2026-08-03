/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_actor.c — the actor pool. See m64_actor.h for the model.
 *
 * Two intrusive singly-linked lists share the `next` field of M64Actor: a
 * free list (`g_free_head`) and one list per category (`g_category_head`).
 * A slot is in exactly one of them at a time, so this costs nothing beyond
 * the one int16_t already in the struct.
 */

#include "m64_actor.h"

#include <stddef.h>
#include <string.h>

static const M64ActorProfile *g_profiles;
static uint16_t g_profile_count;

static M64Actor *g_pool;
static uint16_t g_pool_capacity;
static int16_t g_free_head = -1;
static int16_t g_category_head[M64_ACTOR_CATEGORY_COUNT];

static inline M64ActorHandle make_handle(uint16_t idx, uint16_t gen)
{
    return ((uint32_t)gen << 16) | idx;
}

void m64_actor_system_init(const M64ActorProfile *profiles, uint16_t profile_count,
                           M64Actor *pool, uint16_t pool_capacity)
{
    assertf(pool_capacity > 0, "m64_actor: empty pool");
    assertf(pool_capacity <= 32767, "m64_actor: pool_capacity %u exceeds int16_t index range",
            pool_capacity);

    for (uint16_t i = 0; i < profile_count; i++) {
        assertf(profiles[i].state_size <= M64_ACTOR_STATE_MAX,
                "m64_actor: profile '%s' state_size %u > M64_ACTOR_STATE_MAX %u",
                profiles[i].name ? profiles[i].name : "?",
                profiles[i].state_size, M64_ACTOR_STATE_MAX);
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
        m64_transform_init(&pool[i].xform);
    }
    g_free_head = 0;

    for (int c = 0; c < M64_ACTOR_CATEGORY_COUNT; c++) g_category_head[c] = -1;
}

M64ActorHandle m64_actor_spawn(uint16_t profile_id, fm_vec3_t pos, float yaw)
{
    assertf(profile_id < g_profile_count, "m64_actor: bad profile_id %u", profile_id);
    if (g_free_head < 0) return M64_ACTOR_HANDLE_NONE;

    int16_t idx = g_free_head;
    M64Actor *a = &g_pool[idx];
    g_free_head = a->next;

    const M64ActorProfile *prof = &g_profiles[profile_id];

    a->profile_id = profile_id;
    a->category = prof->category;
    a->flags = prof->default_flags;
    a->room_id = M64_ACTOR_ROOM_NONE;
    a->health = -1;
    a->velocity = (fm_vec3_t){ { 0, 0, 0 } };
    memset(a->state, 0, M64_ACTOR_STATE_MAX);

    a->xform.pos = pos;
    a->xform.scale = (fm_vec3_t){ { 1, 1, 1 } };
    a->xform.rot_axis = (fm_vec3_t){ { 0, 1, 0 } };
    a->xform.rot_angle = yaw;

    /* Most-recently-spawned first. O(1) here is why m64_actor_despawn pays
     * for an O(n) unlink instead: the list has no back-pointers. */
    a->next = g_category_head[prof->category];
    g_category_head[prof->category] = idx;

    if (prof->init) prof->init(a);

    return make_handle((uint16_t)idx, a->generation);
}

M64ActorHandle m64_actor_spawn_in_room(uint16_t profile_id, fm_vec3_t pos, float yaw, uint8_t room_id)
{
    M64ActorHandle h = m64_actor_spawn(profile_id, pos, yaw);
    if (h == M64_ACTOR_HANDLE_NONE) return h;
    M64Actor *a = m64_actor_resolve(h);
    /* m64_actor_spawn never returns NONE alongside a valid handle, but assert
     * the resolve anyway — a stale handle would silently despawn the wrong
     * room's actors. */
    assertf(a != NULL, "m64_actor: spawn returned a handle that does not resolve");
    a->room_id = room_id;
    return h;
}

void m64_actor_despawn(M64ActorHandle h)
{
    M64Actor *a = m64_actor_resolve(h);
    if (!a) return;
    uint16_t idx = (uint16_t)(h & 0xFFFFu);

    const M64ActorProfile *prof = &g_profiles[a->profile_id];
    if (prof->destroy) prof->destroy(a);

    int16_t *link = &g_category_head[a->category];
    while (*link != -1 && *link != (int16_t)idx) link = &g_pool[*link].next;
    if (*link == (int16_t)idx) *link = a->next;

    a->generation++;
    a->next = g_free_head;
    g_free_head = (int16_t)idx;
}

M64Actor *m64_actor_resolve(M64ActorHandle h)
{
    if (h == M64_ACTOR_HANDLE_NONE) return NULL;
    uint16_t idx = (uint16_t)(h & 0xFFFFu);
    uint16_t gen = (uint16_t)(h >> 16);
    if (idx >= g_pool_capacity) return NULL;
    M64Actor *a = &g_pool[idx];
    return (a->generation == gen) ? a : NULL;
}

M64ActorHandle m64_actor_handle_of(const M64Actor *a)
{
    ptrdiff_t idx = a - g_pool;
    assertf(idx >= 0 && idx < g_pool_capacity,
            "m64_actor: handle_of called on a pointer not in this pool");
    return make_handle((uint16_t)idx, a->generation);
}

void m64_actor_update_all(float dt)
{
    for (int c = 0; c < M64_ACTOR_CATEGORY_COUNT; c++) {
        int16_t idx = g_category_head[c];
        while (idx != -1) {
            M64Actor *a = &g_pool[idx];
            /* Captured before calling update: if the actor despawns itself,
             * a->next would otherwise read from a slot already back on the
             * free list. Another actor ahead in the same list despawning a
             * DIFFERENT actor is not handled — deferred despawn would be the
             * fix if that turns out to matter in practice. */
            int16_t next_idx = a->next;
            if (!(a->flags & M64_ACTOR_FLAG_PAUSED)) {
                const M64ActorProfile *prof = &g_profiles[a->profile_id];
                if (prof->update) prof->update(a, dt);
            }
            idx = next_idx;
        }
    }
}

void m64_actor_draw_all(void)
{
    for (int c = 0; c < M64_ACTOR_CATEGORY_COUNT; c++) {
        for (int16_t idx = g_category_head[c]; idx != -1; idx = g_pool[idx].next) {
            M64Actor *a = &g_pool[idx];
            if (a->flags & M64_ACTOR_FLAG_NO_DRAW) continue;
            const M64ActorProfile *prof = &g_profiles[a->profile_id];
            if (!prof->draw) continue;
            /* The system applies the actor's world transform so every draw
             * callback starts from the same place OoT's does: geometry drawn
             * in the actor's own local space. */
            m64_transform_push(&a->xform);
            prof->draw(a);
            m64_transform_pop();
        }
    }
}

M64Actor *m64_actor_first(uint8_t category)
{
    if (category >= M64_ACTOR_CATEGORY_COUNT) return NULL;
    int16_t idx = g_category_head[category];
    return idx == -1 ? NULL : &g_pool[idx];
}

M64Actor *m64_actor_next(M64Actor *cur)
{
    if (!cur) return NULL;
    int16_t idx = cur->next;
    return idx == -1 ? NULL : &g_pool[idx];
}

uint16_t m64_actor_count(uint8_t category)
{
    uint16_t n = 0;
    if (category > M64_ACTOR_CATEGORY_COUNT) return 0;
    int c0 = (category == M64_ACTOR_CATEGORY_COUNT) ? 0 : category;
    int c1 = (category == M64_ACTOR_CATEGORY_COUNT) ? M64_ACTOR_CATEGORY_COUNT - 1 : category;
    for (int c = c0; c <= c1; c++)
        for (int16_t idx = g_category_head[c]; idx != -1; idx = g_pool[idx].next) n++;
    return n;
}
