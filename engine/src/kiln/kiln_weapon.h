/* SPDX-License-Identifier: MIT
 *
 * kiln_weapon.h — weapon state machine. A Quake-style hitscan weapon
 * system, separate from the actor system because weapons are not actors
 * — they are state owned by the player.
 *
 * ── Pure state, no rendering ───────────────────────────────────────────
 * The module tracks ammo, cooldown, and reload timing. The game code
 * reads the state for HUD display and triggers fig_sound_play / hitscan
 * rays when fig_weapon_fire returns 1. A viewmodel (first-person weapon
 * mesh) is a future addition; for now the HUD crosshair is the only
 * visual feedback.
 *
 * ── Why not an actor ───────────────────────────────────────────────────
 * The weapon has no world position, no category, no draw callback, and
 * no per-type state block — it lives in the player's state or in a
 * game-level struct. Making it an actor would add pool bookkeeping
 * for a thing that is always exactly one per player and never appears
 * in the world.
 */
#ifndef FIG_WEAPON_H
#define FIG_WEAPON_H


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

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FIG_WPN_IDLE = 0,
    FIG_WPN_FIRING,
    FIG_WPN_RELOADING,
} FigWeaponState;

typedef struct {
    int           ammo;           /**< reserve ammo (not in magazine)          */
    int           max_ammo;        /**< reserve cap                              */
    int           magazine_size;   /**< rounds per magazine                      */
    int           magazine;        /**< rounds currently loaded                   */
    float         fire_cooldown;   /**< seconds between shots                    */
    float         reload_time;     /**< seconds to reload                        */
    float         timer;           /**< counts down: cooldown or reload remaining */
    FigWeaponState state;          /**< current state                            */
} FigWeapon;

/** Initialise a weapon. Sets ammo to `magazine_size` (one full mag loaded),
 *  no reserve. Call at boot or on weapon switch. */
void fig_weapon_init(FigWeapon *w, int magazine_size,
                     float fire_cooldown, float reload_time);

/** Returns 1 if a shot can be fired now (IDLE + magazine > 0). */
int fig_weapon_can_fire(const FigWeapon *w);

/** Attempt to fire. If successful: decrements magazine, sets state to
 *  FIRING, starts cooldown timer, returns 1. The caller does the hitscan
 *  and plays the sound. If not (cooldown, reloading, empty): returns 0. */
int fig_weapon_fire(FigWeapon *w);

/** Begin a reload: if magazine < magazine_size and ammo > 0, sets state
 *  to RELOADING and starts the reload timer. Returns 0 if no reload is
 *  possible (already full, no reserve, already reloading). */
int fig_weapon_reload(FigWeapon *w);

/** Advance the state machine by `dt` seconds. Transitions FIRING→IDLE
 *  when cooldown elapses, RELOADING→IDLE when reload elapses (moving
 *  rounds from reserve to magazine). */
void fig_weapon_update(FigWeapon *w, float dt);

#ifdef __cplusplus
}
#endif

#endif /* FIG_WEAPON_H */