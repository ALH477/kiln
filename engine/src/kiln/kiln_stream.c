/* SPDX-License-Identifier: MIT
 *
 * kiln_stream.c — see kiln_stream.h for the model.
 *
 * One flat array of KILN_STREAM_MAX_PENDING slots, same shape as
 * kiln_event.c's flat pool: O(n) scans throughout, which at this size (64)
 * is a few hundred cycles on the VR4300 — cheaper than a sorted structure
 * would be, and simpler to get right, which matters more for a pacer whose
 * whole job is not silently doing the wrong thing under pressure.
 */

#include "kiln_stream.h"

#include <string.h>
#include <libdragon.h>

#define IS_LIVE(st) ((st) != KILN_STREAM_SLOT_FREE)

static KilnStreamHandle make_handle(int index, uint8_t generation)
{
    return (KilnStreamHandle)(((uint32_t)(index + 1) & 0xFFFFu) |
                             ((uint32_t)generation << 16));
}

static int handle_index(KilnStreamHandle h)
{
    return (int)(h & 0xFFFFu) - 1;
}

static uint8_t handle_gen(KilnStreamHandle h)
{
    return (uint8_t)((h >> 16) & 0xFFu);
}

/* Slot at `idx` still matches the handle that named it. */
static int handle_valid(const KilnStream *s, KilnStreamHandle h)
{
    if (h == KILN_STREAM_HANDLE_INVALID) return 0;
    int idx = handle_index(h);
    if (idx < 0 || idx >= KILN_STREAM_MAX_PENDING) return 0;
    if (!IS_LIVE(s->slots[idx].state)) return 0;
    return s->slots[idx].generation == handle_gen(h);
}

/* "a" outranks "b": higher urgency wins; within a tier, smaller rank wins. */
static int outranks(KilnStreamUrgency a_urg, float a_rank,
                    KilnStreamUrgency b_urg, float b_rank)
{
    if (a_urg != b_urg) return a_urg > b_urg;
    return a_rank < b_rank;
}

static int find_free_slot(const KilnStream *s)
{
    for (int i = 0; i < KILN_STREAM_MAX_PENDING; i++)
        if (s->slots[i].state == KILN_STREAM_SLOT_FREE) return i;
    return -1;
}

static int find_by_key_tag(const KilnStream *s, const char *key, void *tag)
{
    for (int i = 0; i < KILN_STREAM_MAX_PENDING; i++) {
        const KilnStreamSlot *sl = &s->slots[i];
        if (!IS_LIVE(sl->state)) continue;
        if (sl->tag == tag && strcmp(sl->key, key) == 0) return i;
    }
    return -1;
}

/* Least-important PENDING slot (lowest urgency, then largest rank — the
 * most expendable). ADMITTED slots are never candidates: they were already
 * chosen this frame. Mirrors kiln_event_post's victim search exactly. */
static int find_eviction_victim(const KilnStream *s)
{
    int victim = -1;
    KilnStreamUrgency v_urg = (KilnStreamUrgency)0x7F;
    float v_rank = -1e30f;
    for (int i = 0; i < KILN_STREAM_MAX_PENDING; i++) {
        const KilnStreamSlot *sl = &s->slots[i];
        if (sl->state != KILN_STREAM_SLOT_PENDING) continue;
        if (victim < 0 || outranks(v_urg, v_rank, sl->urgency, sl->rank)) {
            victim = i;
            v_urg = sl->urgency;
            v_rank = sl->rank;
        }
    }
    return victim;
}

static uint8_t occupancy(const KilnStream *s)
{
    uint8_t n = 0;
    for (int i = 0; i < KILN_STREAM_MAX_PENDING; i++)
        if (IS_LIVE(s->slots[i].state)) n++;
    return n;
}

void kiln_stream_init(KilnStream *s, KilnStreamBudget budget)
{
    memset(s, 0, sizeof(*s));
    s->budget = budget;
}

KilnStreamHandle kiln_stream_request(KilnStream *s, const char *key,
                                     KilnStreamUrgency urgency, float rank,
                                     uint32_t byte_cost, void *tag)
{
    int idx = find_by_key_tag(s, key, tag);
    if (idx >= 0) {
        /* Idempotent refresh — a still-outstanding request just gets more
         * (or less) urgent, never duplicated. */
        s->slots[idx].urgency = urgency;
        s->slots[idx].rank = rank;
        s->slots[idx].byte_cost = byte_cost;
        return make_handle(idx, s->slots[idx].generation);
    }

    idx = find_free_slot(s);
    if (idx < 0) {
        int victim = find_eviction_victim(s);
        if (victim < 0 ||
            !outranks(urgency, rank, s->slots[victim].urgency, s->slots[victim].rank)) {
            debugf("kiln_stream: drop request '%s' (pool full, no lower-"
                   "priority PENDING slot to evict)\n", key);
            s->dropped_total++;
            return KILN_STREAM_HANDLE_INVALID;
        }
        debugf("kiln_stream: evict '%s' to make room for '%s'\n",
               s->slots[victim].key, key);
        idx = victim;
    }

    KilnStreamSlot *sl = &s->slots[idx];
    sl->generation++;
    strncpy(sl->key, key, KILN_STREAM_MAX_KEY_LEN - 1);
    sl->key[KILN_STREAM_MAX_KEY_LEN - 1] = '\0';
    sl->byte_cost = byte_cost;
    sl->urgency = urgency;
    sl->rank = rank;
    sl->tag = tag;
    sl->state = KILN_STREAM_SLOT_PENDING;

    uint8_t occ = occupancy(s);
    if (occ > s->high_water) s->high_water = occ;

    return make_handle(idx, sl->generation);
}

int kiln_stream_cancel(KilnStream *s, KilnStreamHandle h)
{
    if (!handle_valid(s, h)) return -1;
    KilnStreamSlot *sl = &s->slots[handle_index(h)];
    sl->state = KILN_STREAM_SLOT_FREE;
    sl->tag = NULL;
    return 0;
}

void kiln_stream_frame_begin(KilnStream *s)
{
    s->admits_this_frame = 0;
    s->bytes_admitted_this_frame = 0;

    for (;;) {
        if (s->admits_this_frame >= s->budget.max_admits_per_frame) break;

        int best = -1;
        for (int i = 0; i < KILN_STREAM_MAX_PENDING; i++) {
            if (s->slots[i].state != KILN_STREAM_SLOT_PENDING) continue;
            if (best < 0 || outranks(s->slots[i].urgency, s->slots[i].rank,
                                     s->slots[best].urgency, s->slots[best].rank))
                best = i;
        }
        if (best < 0) break;  /* nothing left waiting */

        uint32_t cost = s->slots[best].byte_cost;
        int fits = (s->bytes_admitted_this_frame + cost <= s->budget.max_bytes_per_frame);
        /* Force through a single oversized request rather than starve it
         * forever — see the file comment. */
        int force = (s->admits_this_frame == 0 && !fits);
        if (!fits && !force) break;

        s->slots[best].state = KILN_STREAM_SLOT_ADMITTED;
        s->bytes_admitted_this_frame += cost;
        s->admits_this_frame++;
    }
}

KilnStreamSlot *kiln_stream_first_admitted(KilnStream *s)
{
    for (int i = 0; i < KILN_STREAM_MAX_PENDING; i++)
        if (s->slots[i].state == KILN_STREAM_SLOT_ADMITTED) return &s->slots[i];
    return NULL;
}

KilnStreamSlot *kiln_stream_next_admitted(KilnStream *s, KilnStreamSlot *cur)
{
    int start = (int)(cur - s->slots) + 1;
    for (int i = start; i < KILN_STREAM_MAX_PENDING; i++)
        if (s->slots[i].state == KILN_STREAM_SLOT_ADMITTED) return &s->slots[i];
    return NULL;
}

void kiln_stream_complete(KilnStream *s, KilnStreamHandle h)
{
    if (!handle_valid(s, h)) return;
    KilnStreamSlot *sl = &s->slots[handle_index(h)];
    sl->state = KILN_STREAM_SLOT_FREE;
    sl->tag = NULL;
}

uint8_t kiln_stream_pending_count(const KilnStream *s)
{
    return occupancy(s);
}

uint32_t kiln_stream_bytes_admitted_this_frame(const KilnStream *s)
{
    return s->bytes_admitted_this_frame;
}

uint8_t kiln_stream_admits_this_frame(const KilnStream *s)
{
    return s->admits_this_frame;
}

uint32_t kiln_stream_dropped_total(const KilnStream *s)
{
    return s->dropped_total;
}

uint16_t kiln_stream_high_water(const KilnStream *s)
{
    return s->high_water;
}
