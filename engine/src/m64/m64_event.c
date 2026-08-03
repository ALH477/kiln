/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_event.c — see m64_event.h for the model.
 *
 * One flat array of M64_EVENT_MAX slots. A slot is active iff `target !=
 * M64_ACTOR_HANDLE_NONE`. process() walks the array, decrements `delay_ms`
 * by the frame's ms, and dispatches any event that has reached zero (or
 * below — a long frame can blow straight through a small delay). Dispatched
 * events are cleared by setting target back to NONE.
 *
 * The walk is O(M64_EVENT_MAX) per frame, which at 256 slots is a few hundred
 * cycles on the VR4300 — cheaper than a sorted list would be, and it keeps
 * post() O(1) (linear search for a free slot is also O(256), same order).
 * No fancy data structure earns its keep at this size.
 */

#include "m64_event.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <libdragon.h>

typedef struct {
    M64ActorHandle target;     /* NONE => free slot                    */
    uint16_t       event_id;
    int32_t        delay_ms;   /* decremented by process(); fires <=0  */
    uint8_t        argc;
    uint8_t        priority;
    int32_t        args[M64_EVENT_ARG_MAX];
} M64EventSlot;

static M64EventSlot g_slots[M64_EVENT_MAX];
static uint16_t     g_active;

void m64_event_init(void)
{
    for (int i = 0; i < M64_EVENT_MAX; i++) {
        g_slots[i].target = M64_ACTOR_HANDLE_NONE;
        g_slots[i].argc = 0;
        g_slots[i].priority = 0;
        g_slots[i].delay_ms = 0;
    }
    g_active = 0;
}

static int find_free_slot(void)
{
    for (int i = 0; i < M64_EVENT_MAX; i++)
        if (g_slots[i].target == M64_ACTOR_HANDLE_NONE) return i;
    return -1;
}

int m64_event_post(M64ActorHandle target, uint16_t event_id, int delay_ms,
                   const int32_t *args, uint8_t argc, uint8_t priority)
{
    if (target == M64_ACTOR_HANDLE_NONE) return -1;
    if (argc > M64_EVENT_ARG_MAX) argc = M64_EVENT_ARG_MAX;

    int slot = find_free_slot();
    if (slot < 0) {
        /* Pool full: evict the lowest-priority active event that has the
         * largest remaining delay (most expendable: least urgent AND
         * furthest from firing). Only evict if the new event is strictly
         * higher priority; otherwise drop the new event. */
        int victim = -1;
        uint8_t vprio = 0xFF;
        int32_t vdelay = INT32_MIN;
        for (int i = 0; i < M64_EVENT_MAX; i++) {
            M64EventSlot *s = &g_slots[i];
            if (s->target == M64_ACTOR_HANDLE_NONE) continue;
            if (s->priority < vprio ||
                (s->priority == vprio && s->delay_ms > vdelay)) {
                vprio = s->priority;
                vdelay = s->delay_ms;
                victim = i;
            }
        }
        if (victim < 0 || vprio >= priority) {
            debugf("m64_event: drop event %u pri %u (pool full, "
                   "lowest pri %u)\n", event_id, priority, vprio);
            return -1;
        }
        debugf("m64_event: evict event %u pri %u to make room for %u pri %u\n",
               g_slots[victim].event_id, vprio, event_id, priority);
        slot = victim;
    }

    M64EventSlot *s = &g_slots[slot];
    if (s->target == M64_ACTOR_HANDLE_NONE) g_active++;
    s->target = target;
    s->event_id = event_id;
    s->delay_ms = delay_ms;
    s->argc = argc;
    s->priority = priority;
    if (argc && args) memcpy(s->args, args, sizeof(int32_t) * argc);
    else memset(s->args, 0, sizeof(s->args));
    return 0;
}

void m64_event_process(float dt)
{
    if (g_active == 0) return;
    int32_t ms = (int32_t)(dt * 1000.0f);
    if (ms <= 0) ms = 1;

    /* Walk forwards; m64_actor_dispatch_event may post new events, which
     * find their own free slots — we won't re-process them this frame
     * because their delay_ms is whatever the poster set, not yet decremented
     * (and they may land in slots we've already passed, which is fine). */
    for (int i = 0; i < M64_EVENT_MAX; i++) {
        M64EventSlot *s = &g_slots[i];
        if (s->target == M64_ACTOR_HANDLE_NONE) continue;

        s->delay_ms -= ms;
        if (s->delay_ms > 0) continue;

        M64Actor *a = m64_actor_resolve(s->target);
        if (a) {
            m64_actor_dispatch_event(a, s->event_id, s->args, s->argc);
        } /* else: target despawned before fire — drop silently */
        s->target = M64_ACTOR_HANDLE_NONE;
        g_active--;
    }
}

uint16_t m64_event_count(void)
{
    return g_active;
}