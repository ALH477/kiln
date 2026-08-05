/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_trigger.h — trigger volumes. AABB regions in the world that fire
 * events when the player enters them. Modelled on Half-Life's
 * trigger_once, trigger_multiple, and trigger_push entities.
 *
 * ── Why not actors ──────────────────────────────────────────────────────
 * Triggers are invisible, have no update logic (only an AABB test), and
 * fire events on the player, not on themselves. Making them actors would
 * waste a pool slot and a category-list walk per frame for something that
 * is a simple box test. A dedicated array of 32 triggers (~36 bytes each)
 * is cheaper and clearer.
 *
 * ── Event posting ───────────────────────────────────────────────────────
 * When the player enters a trigger, it posts an event via m64_event_post
 * to the player actor. The game's player event callback dispatches on
 * the event_id (e.g. EV_SPAWN_AMBUSH, EV_DOOR_OPEN). This keeps the
 * trigger system generic — it doesn't know what a "door" is, just that
 * entering this box should tell the player "something happened".
 *
 * ── trigger_push ─────────────────────────────────────────────────────────
 * M64_TRIG_PUSH applies a velocity to the player each frame while the
 * player is inside the volume (jump pads, wind tunnels, conveyor belts).
 * It does NOT post events — it directly modifies a velocity vector the
 * game reads. The game adds this to the player's movement each frame.
 */
#ifndef M64_TRIGGER_H
#define M64_TRIGGER_H

#include <t3d/t3dmath.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M64_TRIGGER_MAX 32

typedef enum {
    M64_TRIG_ONCE = 0,
    M64_TRIG_MULTIPLE,
    M64_TRIG_PUSH,
} M64TrigType;

typedef struct {
    fm_vec3_t mins, maxs;
    uint8_t  type;
    uint16_t event_id;
    int32_t  args[4];
    uint8_t  argc;
    uint8_t  fired;
    uint8_t  active;
    fm_vec3_t push_vel;
} M64Trigger;

void m64_trigger_init(void);

/** Register a trigger. Returns 1 on success, 0 if the array is full. */
int m64_trigger_add(const M64Trigger *t);

/** Per-frame update. Checks all active triggers against `player_pos`.
 *  For ONCE/MULTIPLE: posts `event_id` to `player_handle` when the player
 *  enters the volume. For PUSH: sets `out_push_vel` (accumulates if
 *  multiple push triggers overlap). Call once per frame. */
void m64_trigger_update(fm_vec3_t player_pos, uint32_t player_handle,
                         float dt, fm_vec3_t *out_push_vel);

/** Clear all triggers (call on room unload). */
void m64_trigger_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* M64_TRIGGER_H */