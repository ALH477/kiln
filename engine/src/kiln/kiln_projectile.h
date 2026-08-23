/* SPDX-License-Identifier: MIT
 *
 * kiln_projectile.h — projectile system. A lightweight pool of fast-moving
 * projectiles with ray-cast collision, separate from the actor system.
 *
 * ── Why not actors ──────────────────────────────────────────────────────
 * Projectiles are transient (lifetime < 2 s), fast-moving (500+ u/s),
 * and numerous (a plasma rifle fires 10/sec). Making each one an actor
 * costs pool capacity, category-list overhead, and a full KilnActor
 * header per projectile — 64+ bytes of state block the projectile never
 * uses. A dedicated pool of 32 KilnProjectile structs (~28 bytes each,
 * < 1 KB total) is cheaper and simpler.
 *
 * ── Collision via kiln_clip_ray ──────────────────────────────────────────
 * Each frame, a projectile casts a ray from its current position to
 * pos + vel*dt. If the ray hits a brush, the projectile explodes. If it
 * passes through an enemy's position (distance check), it hits. This is
 * the same hitscan pattern as the FPS's do_hitscan, re-used per-frame
 * instead of per-shot.
 *
 * ── Splash damage ──────────────────────────────────────────────────────
 * Rockets and grenades deal splash damage: on impact, every enemy
 * within `radius` takes damage scaled by distance. The game's enemy
 * list is walked linearly (same as targeting and hitscan — OoT enemy
 * counts per room are small enough that a spatial index costs more than
 * it saves).
 */
#ifndef KILN_PROJECTILE_H
#define KILN_PROJECTILE_H

#include <t3d/t3dmath.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KILN_PROJECTILE_MAX 32

typedef enum {
    KILN_PROJ_ROCKET = 0,
    KILN_PROJ_PLASMA,
    KILN_PROJ_GRENADE,
} KilnProjType;

typedef struct {
    fm_vec3_t pos;
    fm_vec3_t vel;
    float    lifetime;
    float    radius;
    int      damage;
    uint8_t  type;
    uint8_t  active;
} KilnProjectile;

void kiln_projectile_init(void);

/** Spawn a projectile. Returns 1 on success, 0 if pool full. */
int kiln_projectile_spawn(uint8_t type, fm_vec3_t pos, fm_vec3_t vel,
                          float lifetime, int damage, float radius);

/** Advance all active projectiles by `dt`. Handles movement, ray-cast
 *  collision, enemy hits, and splash damage. Calls the game's damage
 *  callback (set via kiln_projectile_set_damage_fn) on hit. */
void kiln_projectile_update(float dt);

/** Draw all active projectiles in the 3D pass. Small colored cubes
 *  whose color depends on the projectile type. */
void kiln_projectile_draw_all(void);

/** Set the callback invoked when a projectile hits an enemy. The
 *  callback receives the enemy actor handle, the damage, and the hit
 *  position. The game uses this to apply damage and play SFX. */
typedef void (*KilnProjHitFn)(uint16_t enemy_profile_id, fm_vec3_t pos,
                              int damage, float splash_radius);
void kiln_projectile_set_hit_fn(KilnProjHitFn fn);

#ifdef __cplusplus
}
#endif

#endif /* KILN_PROJECTILE_H */