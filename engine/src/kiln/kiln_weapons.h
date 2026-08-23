/* SPDX-License-Identifier: MIT
 *
 * kiln_weapons.h — multi-weapon system. An array of KilnWeapon instances
 * with switching, wrapping the single-weapon kiln_weapon.h state machine.
 *
 * ── Doom weapon wheel ──────────────────────────────────────────────────
 * Doom's weapon switching is instant but has a brief "raise" animation
 * before the weapon can fire. We model that as a switch_timer: after
 * switching, the weapon can't fire for 0.3 s. This prevents accidental
 * double-fire on a D-pad tap and gives the player visual feedback that
 * the switch happened (the HUD ammo readout changes).
 *
 * ── Hitscan vs projectile ───────────────────────────────────────────────
 * Each weapon definition specifies its type: HITSCAN (pistol, shotgun)
 * or PROJECTILE (rocket launcher, plasma rifle). The game's fire
 * function checks the type and either does a ray-cast (hitscan) or
 * spawns a projectile via kiln_projectile_spawn. The weapon system
 * itself doesn't do the ray-cast or spawn — it only manages ammo,
 * cooldown, and switching. The game reads the active weapon's
 * definition and does the appropriate fire action.
 *
 * ── Ammo per weapon ────────────────────────────────────────────────────
 * Each weapon has its own KilnWeapon state (magazine, reserve ammo,
 * cooldown timer). Switching weapons does NOT reset the previous
 * weapon's state — you come back to the same magazine you left. This
 * matches Doom and HL, not modern shooters that auto-reload on switch.
 */
#ifndef KILN_WEAPONS_H
#define KILN_WEAPONS_H

#include <stdint.h>
#include "kiln_weapon.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KILN_WEAPON_SLOTS 4

typedef enum {
    KILN_WTYPE_HITSCAN = 0,
    KILN_WTYPE_PROJECTILE,
} KilnWeaponType;

typedef enum {
    KILN_PROJ_NONE = 0,
    KILN_PROJ_ROCKET_W,
    KILN_PROJ_PLASMA_W,
    KILN_PROJ_GRENADE_W,
} KilnWeaponProj;

typedef struct {
    char name[16];
    uint8_t type;           // KilnWeaponType
    uint8_t proj_type;      // KilnWeaponProj (for PROJECTILE type)
    int magazine_size;
    float fire_cooldown;
    float reload_time;
    int damage;
    float splash_radius;
    const char *sfx_name;
    uint32_t cube_color;    // for HUD weapon indicator
} KilnWeaponDef;

typedef struct {
    KilnWeaponDef defs[KILN_WEAPON_SLOTS];
    KilnWeapon    state[KILN_WEAPON_SLOTS];
    int active_slot;
    int slot_count;
    float switch_timer;
} KilnWeaponSet;

/** Initialise the weapon set with `count` weapon definitions.
 *  Each weapon's KilnWeapon state is initialised with the def's
 *  magazine_size, fire_cooldown, and reload_time. */
void kiln_weapons_init(KilnWeaponSet *ws, const KilnWeaponDef *defs, int count);

/** Switch to slot (0-based). Returns 1 on success, 0 if slot invalid
 *  or already active. Starts the switch timer. */
int kiln_weapons_switch(KilnWeaponSet *ws, int slot);

/** Cycle to the next/previous weapon slot. */
int kiln_weapons_next(KilnWeaponSet *ws);
int kiln_weapons_prev(KilnWeaponSet *ws);

/** Attempt to fire the active weapon. Returns 1 if fired (decrements
 *  magazine, starts cooldown). Returns 0 if cooldown, reloading,
 *  switching, or empty. */
int kiln_weapons_fire(KilnWeaponSet *ws);

/** Reload the active weapon. Returns 1 on success. */
int kiln_weapons_reload(KilnWeaponSet *ws);

/** Advance all weapon states by `dt`. Also decrements the switch
 *  timer. */
void kiln_weapons_update(KilnWeaponSet *ws, float dt);

/** Get the active weapon's definition (or NULL if no weapons). */
const KilnWeaponDef *kiln_weapons_active_def(const KilnWeaponSet *ws);

/** Get the active weapon's mutable state. */
KilnWeapon *kiln_weapons_active_state(KilnWeaponSet *ws);

/** Returns 1 if the weapon can fire right now (IDLE, not switching,
 *  magazine > 0). */
int kiln_weapons_can_fire(const KilnWeaponSet *ws);

#ifdef __cplusplus
}
#endif

#endif /* KILN_WEAPONS_H */