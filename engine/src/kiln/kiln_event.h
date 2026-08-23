/* SPDX-License-Identifier: MIT
 *
 * kiln_event.h — timed event dispatch. A clean-room analogue of id Tech 4's
 * idEvent, reduced to what an N64 game can afford.
 *
 * ── One global queue, not per-actor ──────────────────────────────────────
 * Doom 3 keeps a per-class event queue and a global event scheduler. On N64
 * we keep one flat pool of KILN_EVENT_MAX slots (256 events × ~28 B ≈ 7 KB)
 * and a single kiln_event_process per frame. Per-actor queues would mean
 * per-actor malloc, which the engine deliberately never does (see
 * kiln_actor.h's flat-pool rationale).
 *
 * ── Time is milliseconds, integer ─────────────────────────────────────────
 * `delay_ms` is a signed int so an event due "now" can be posted with 0.
 * process() converts dt (seconds, float) to ms and decrements; events fire
 * when their remaining delay reaches zero or below. No sub-millisecond
 * precision is needed — N64 frame budget is 16.6 ms.
 *
 * ── Pool-full policy: evict the most expendable ──────────────────────────
 * When the pool is full and a new event arrives with priority higher than
 * the lowest-priority queued event, that lowest-priority event is evicted
 * (and debugf'd) and the new event takes its slot. Otherwise the new event
 * is dropped (and debugf'd). This matches Doom3's "drop oldest
 * lowest-priority" intent without tracking per-event insertion order; the
 * tiebreaker among equal-priority events is the largest remaining delay
 * (the one furthest from firing, i.e. least urgent).
 *
 * ── Stale targets are dropped silently ───────────────────────────────────
 * If the target actor has been despawned before the event fires, the event
 * is dropped when it reaches its fire time — no warning, since this is the
 * normal way a queued event becomes irrelevant (a killed actor's pending
 * "play idle anim" event should not log a warning that spoils real bugs).
 */
#ifndef KILN_EVENT_H
#define KILN_EVENT_H

#include <stdint.h>
#include "kiln_actor.h"

#define KILN_EVENT_MAX     256
#define KILN_EVENT_ARG_MAX  4

#ifdef __cplusplus
extern "C" {
#endif

/** Initialise the event pool. Call once at boot, after kiln_actor_system_init. */
void kiln_event_init(void);

/** Queue an event for `target` to be dispatched `delay_ms` later. Up to
 *  KILN_EVENT_ARG_MAX int32_t of opaque payload pass through to the actor's
 *  KilnActorEventFn. `priority` is only consulted when the pool is full.
 *  Returns 0 on success, -1 if the event was dropped (pool full of higher-
 *  priority events). */
int kiln_event_post(KilnActorHandle target, uint16_t event_id, int delay_ms,
                   const int32_t *args, uint8_t argc, uint8_t priority);

/** Advance the queue by `dt` seconds and dispatch any due events to their
 *  target actor's profile event callback. Call once per frame, BEFORE
 *  kiln_actor_update_all — events should land before the actor's own update
 *  so the actor's state machine sees the event this frame. */
void kiln_event_process(float dt);

/** Number of events currently queued. Diagnostic. */
uint16_t kiln_event_count(void);

#ifdef __cplusplus
}
#endif

#endif /* KILN_EVENT_H */