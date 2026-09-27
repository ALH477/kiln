/* SPDX-License-Identifier: MIT
 *
 * kiln_weapons.h — multi-weapon system. An array of FigWeapon instances
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
 * spawns a projectile via fig_projectile_spawn. The weapon system
 * itself doesn't do the ray-cast or spawn — it only manages ammo,
 * cooldown, and switching. The game reads the active weapon's
 * definition and does the appropriate fire action.
 *
 * ── Ammo per weapon ────────────────────────────────────────────────────
 * Each weapon has its own FigWeapon state (magazine, reserve ammo,
 * cooldown timer). Switching weapons does NOT reset the previous
 * weapon's state — you come back to the same magazine you left. This
 * matches Doom and HL, not modern shooters that auto-reload on switch.
 */
#ifndef FIG_WEAPONS_H
#define FIG_WEAPONS_H


/* The prefix migration train (docs/NAMING.md section 9 step 2). Pulled in by
 * every public header (a quoted include, so it resolves both in this tree and
 * in the installed include/kiln prefix) rather than force-included by
 * kiln-inst.mk, because a
 * force-include only reaches builds that include that file — a Nix check or a
 * host build compiling a downstream's sources directly never saw it, and
 * PetaByte-Madness' pm-cine check is what proved that. Deleting the train is
 * still a scripted one-line removal from these headers plus the file itself.
 */
#include "kiln_compat.h"

#include <stdint.h>
#include "kiln_weapon.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FIG_WEAPON_SLOTS 4

typedef enum {
    FIG_WTYPE_HITSCAN = 0,
    FIG_WTYPE_PROJECTILE,
} FigWeaponType;

typedef enum {
    FIG_PROJ_NONE = 0,
    FIG_PROJ_ROCKET_W,
    FIG_PROJ_PLASMA_W,
    FIG_PROJ_GRENADE_W,
} FigWeaponProj;

typedef struct {
    char name[16];
    uint8_t type;           // FigWeaponType
    uint8_t proj_type;      // FigWeaponProj (for PROJECTILE type)
    int magazine_size;
    float fire_cooldown;
    float reload_time;
    int damage;
    float splash_radius;
    const char *sfx_name;
    uint32_t cube_color;    // for HUD weapon indicator
} FigWeaponDef;

typedef struct {
    FigWeaponDef defs[FIG_WEAPON_SLOTS];
    FigWeapon    state[FIG_WEAPON_SLOTS];
    int active_slot;
    int slot_count;
    float switch_timer;
} FigWeaponSet;

/** Initialise the weapon set with `count` weapon definitions.
 *  Each weapon's FigWeapon state is initialised with the def's
 *  magazine_size, fire_cooldown, and reload_time. */
void fig_weapons_init(FigWeaponSet *ws, const FigWeaponDef *defs, int count);

/** Switch to slot (0-based). Returns 1 on success, 0 if slot invalid
 *  or already active. Starts the switch timer. */
int fig_weapons_switch(FigWeaponSet *ws, int slot);

/** Cycle to the next/previous weapon slot. */
int fig_weapons_next(FigWeaponSet *ws);
int fig_weapons_prev(FigWeaponSet *ws);

/** Attempt to fire the active weapon. Returns 1 if fired (decrements
 *  magazine, starts cooldown). Returns 0 if cooldown, reloading,
 *  switching, or empty. */
int fig_weapons_fire(FigWeaponSet *ws);

/** Reload the active weapon. Returns 1 on success. */
int fig_weapons_reload(FigWeaponSet *ws);

/** Advance all weapon states by `dt`. Also decrements the switch
 *  timer. */
void fig_weapons_update(FigWeaponSet *ws, float dt);

/** Get the active weapon's definition (or NULL if no weapons). */
const FigWeaponDef *fig_weapons_active_def(const FigWeaponSet *ws);

/** Get the active weapon's mutable state. */
FigWeapon *fig_weapons_active_state(FigWeaponSet *ws);

/** Returns 1 if the weapon can fire right now (IDLE, not switching,
 *  magazine > 0). */
int fig_weapons_can_fire(const FigWeaponSet *ws);

#ifdef __cplusplus
}
#endif

#endif /* FIG_WEAPONS_H */