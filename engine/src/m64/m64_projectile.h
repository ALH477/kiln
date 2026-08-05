/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_projectile.h — projectile system. A lightweight pool of fast-moving
 * projectiles with ray-cast collision, separate from the actor system.
 *
 * ── Why not actors ──────────────────────────────────────────────────────
 * Projectiles are transient (lifetime < 2 s), fast-moving (500+ u/s),
 * and numerous (a plasma rifle fires 10/sec). Making each one an actor
 * costs pool capacity, category-list overhead, and a full M64Actor
 * header per projectile — 64+ bytes of state block the projectile never
 * uses. A dedicated pool of 32 M64Projectile structs (~28 bytes each,
 * < 1 KB total) is cheaper and simpler.
 *
 * ── Collision via m64_clip_ray ──────────────────────────────────────────
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
#ifndef M64_PROJECTILE_H
#define M64_PROJECTILE_H

#include <t3d/t3dmath.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M64_PROJECTILE_MAX 32

typedef enum {
    M64_PROJ_ROCKET = 0,
    M64_PROJ_PLASMA,
    M64_PROJ_GRENADE,
} M64ProjType;

typedef struct {
    fm_vec3_t pos;
    fm_vec3_t vel;
    float    lifetime;
    float    radius;
    int      damage;
    uint8_t  type;
    uint8_t  active;
} M64Projectile;

void m64_projectile_init(void);

/** Spawn a projectile. Returns 1 on success, 0 if pool full. */
int m64_projectile_spawn(uint8_t type, fm_vec3_t pos, fm_vec3_t vel,
                          float lifetime, int damage, float radius);

/** Advance all active projectiles by `dt`. Handles movement, ray-cast
 *  collision, enemy hits, and splash damage. Calls the game's damage
 *  callback (set via m64_projectile_set_damage_fn) on hit. */
void m64_projectile_update(float dt);

/** Draw all active projectiles in the 3D pass. Small colored cubes
 *  whose color depends on the projectile type. */
void m64_projectile_draw_all(void);

/** Set the callback invoked when a projectile hits an enemy. The
 *  callback receives the enemy actor handle, the damage, and the hit
 *  position. The game uses this to apply damage and play SFX. */
typedef void (*M64ProjHitFn)(uint16_t enemy_profile_id, fm_vec3_t pos,
                              int damage, float splash_radius);
void m64_projectile_set_hit_fn(M64ProjHitFn fn);

#ifdef __cplusplus
}
#endif

#endif /* M64_PROJECTILE_H */