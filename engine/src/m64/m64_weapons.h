/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_weapons.h — multi-weapon system. An array of M64Weapon instances
 * with switching, wrapping the single-weapon m64_weapon.h state machine.
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
 * spawns a projectile via m64_projectile_spawn. The weapon system
 * itself doesn't do the ray-cast or spawn — it only manages ammo,
 * cooldown, and switching. The game reads the active weapon's
 * definition and does the appropriate fire action.
 *
 * ── Ammo per weapon ────────────────────────────────────────────────────
 * Each weapon has its own M64Weapon state (magazine, reserve ammo,
 * cooldown timer). Switching weapons does NOT reset the previous
 * weapon's state — you come back to the same magazine you left. This
 * matches Doom and HL, not modern shooters that auto-reload on switch.
 */
#ifndef M64_WEAPONS_H
#define M64_WEAPONS_H

#include <stdint.h>
#include "m64_weapon.h"

#ifdef __cplusplus
extern "C" {
#endif

#define M64_WEAPON_SLOTS 4

typedef enum {
    M64_WTYPE_HITSCAN = 0,
    M64_WTYPE_PROJECTILE,
} M64WeaponType;

typedef enum {
    M64_PROJ_NONE = 0,
    M64_PROJ_ROCKET_W,
    M64_PROJ_PLASMA_W,
    M64_PROJ_GRENADE_W,
} M64WeaponProj;

typedef struct {
    char name[16];
    uint8_t type;           // M64WeaponType
    uint8_t proj_type;      // M64WeaponProj (for PROJECTILE type)
    int magazine_size;
    float fire_cooldown;
    float reload_time;
    int damage;
    float splash_radius;
    const char *sfx_name;
    uint32_t cube_color;    // for HUD weapon indicator
} M64WeaponDef;

typedef struct {
    M64WeaponDef defs[M64_WEAPON_SLOTS];
    M64Weapon    state[M64_WEAPON_SLOTS];
    int active_slot;
    int slot_count;
    float switch_timer;
} M64WeaponSet;

/** Initialise the weapon set with `count` weapon definitions.
 *  Each weapon's M64Weapon state is initialised with the def's
 *  magazine_size, fire_cooldown, and reload_time. */
void m64_weapons_init(M64WeaponSet *ws, const M64WeaponDef *defs, int count);

/** Switch to slot (0-based). Returns 1 on success, 0 if slot invalid
 *  or already active. Starts the switch timer. */
int m64_weapons_switch(M64WeaponSet *ws, int slot);

/** Cycle to the next/previous weapon slot. */
int m64_weapons_next(M64WeaponSet *ws);
int m64_weapons_prev(M64WeaponSet *ws);

/** Attempt to fire the active weapon. Returns 1 if fired (decrements
 *  magazine, starts cooldown). Returns 0 if cooldown, reloading,
 *  switching, or empty. */
int m64_weapons_fire(M64WeaponSet *ws);

/** Reload the active weapon. Returns 1 on success. */
int m64_weapons_reload(M64WeaponSet *ws);

/** Advance all weapon states by `dt`. Also decrements the switch
 *  timer. */
void m64_weapons_update(M64WeaponSet *ws, float dt);

/** Get the active weapon's definition (or NULL if no weapons). */
const M64WeaponDef *m64_weapons_active_def(const M64WeaponSet *ws);

/** Get the active weapon's mutable state. */
M64Weapon *m64_weapons_active_state(M64WeaponSet *ws);

/** Returns 1 if the weapon can fire right now (IDLE, not switching,
 *  magazine > 0). */
int m64_weapons_can_fire(const M64WeaponSet *ws);

#ifdef __cplusplus
}
#endif

#endif /* M64_WEAPONS_H */