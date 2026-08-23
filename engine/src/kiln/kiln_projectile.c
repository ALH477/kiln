/* SPDX-License-Identifier: MIT
 *
 * kiln_projectile.c — see kiln_projectile.h for the model.
 */

#include "kiln_projectile.h"
#include "kiln_clip.h"
#include "kiln_actor.h"

#include <libdragon.h>
#include <fmath.h>
#include <string.h>

static KilnProjectile g_pool[KILN_PROJECTILE_MAX];
static KilnProjHitFn  g_hit_fn;

static const uint8_t CUBE_TRIS[12][3] = {
    {0,1,2},{2,3,0}, {4,6,5},{6,4,7},
    {0,4,5},{5,1,0}, {1,5,6},{6,2,1},
    {2,6,7},{7,3,2}, {3,7,4},{4,0,3},
};

static T3DVertPacked *g_proj_verts[3];

static T3DVertPacked *make_proj_cube(int16_t half, uint32_t rgba)
{
    T3DVertPacked *v = malloc_uncached(sizeof(T3DVertPacked) * 4);
    const int16_t s = half;
    const int16_t c[8][3] = {
        {-s,-s,-s},{ s,-s,-s},{ s, s,-s},{-s, s,-s},
        {-s,-s, s},{ s,-s, s},{ s, s, s},{-s, s, s},
    };
    for (int i = 0; i < 8; i += 2) {
        fm_vec3_t na = {{ (float)c[i][0],   (float)c[i][1],   (float)c[i][2]   }};
        fm_vec3_t nb = {{ (float)c[i+1][0], (float)c[i+1][1], (float)c[i+1][2] }};
        fm_vec3_norm(&na, &na);
        fm_vec3_norm(&nb, &nb);
        v[i / 2] = (T3DVertPacked){
            .posA = { c[i][0],   c[i][1],   c[i][2]   }, .rgbaA = rgba,
            .normA = t3d_vert_pack_normal(&na),
            .posB = { c[i+1][0], c[i+1][1], c[i+1][2] }, .rgbaB = rgba,
            .normB = t3d_vert_pack_normal(&nb),
        };
    }
    return v;
}

void kiln_projectile_init(void)
{
    memset(g_pool, 0, sizeof(g_pool));
    g_hit_fn = NULL;
    g_proj_verts[KILN_PROJ_ROCKET]  = make_proj_cube(4, 0xFF4444FF);
    g_proj_verts[KILN_PROJ_PLASMA]  = make_proj_cube(3, 0x44FFFFFF);
    g_proj_verts[KILN_PROJ_GRENADE] = make_proj_cube(4, 0x88FF44FF);
}

void kiln_projectile_set_hit_fn(KilnProjHitFn fn) { g_hit_fn = fn; }

int kiln_projectile_spawn(uint8_t type, fm_vec3_t pos, fm_vec3_t vel,
                          float lifetime, int damage, float radius)
{
    for (int i = 0; i < KILN_PROJECTILE_MAX; i++) {
        if (!g_pool[i].active) {
            g_pool[i].pos = pos;
            g_pool[i].vel = vel;
            g_pool[i].lifetime = lifetime;
            g_pool[i].damage = damage;
            g_pool[i].radius = radius;
            g_pool[i].type = type;
            g_pool[i].active = 1;
            return 1;
        }
    }
    return 0;
}

static void explode(fm_vec3_t pos, int damage, float radius)
{
    if (!g_hit_fn) return;
    for (int cat = KILN_ACTOR_CAT_ENEMY; cat <= KILN_ACTOR_CAT_ENEMY; cat++) {
        for (KilnActor *a = kiln_actor_first((uint8_t)cat); a; a = kiln_actor_next(a)) {
            fm_vec3_t d = {{ a->xform.pos.v[0] - pos.v[0],
                              a->xform.pos.v[1] - pos.v[1],
                              a->xform.pos.v[2] - pos.v[2] }};
            float dist = fm_vec3_len(&d);
            if (dist < radius) {
                int dmg = (int)(damage * (1.0f - dist / radius));
                if (dmg < 1) dmg = 1;
                g_hit_fn(a->profile_id, pos, dmg, 0.0f);
            }
        }
    }
}

void kiln_projectile_update(float dt)
{
    for (int i = 0; i < KILN_PROJECTILE_MAX; i++) {
        KilnProjectile *p = &g_pool[i];
        if (!p->active) continue;

        p->lifetime -= dt;
        if (p->lifetime <= 0.0f) {
            if (p->radius > 0) explode(p->pos, p->damage, p->radius);
            p->active = 0;
            continue;
        }

        fm_vec3_t new_pos = {{ p->pos.v[0] + p->vel.v[0] * dt,
                                p->pos.v[1] + p->vel.v[1] * dt,
                                p->pos.v[2] + p->vel.v[2] * dt }};

        if (p->type == KILN_PROJ_GRENADE) {
            p->vel.v[1] -= 500.0f * dt;
            fm_vec3_t vel = {{ p->vel.v[0] * dt, p->vel.v[1] * dt, p->vel.v[2] * dt }};
            p->pos = kiln_clip_slide(p->pos, vel,
                                     (fm_vec3_t){{-2,-2,-2}}, (fm_vec3_t){{2,2,2}}, 4);
        } else {
            fm_vec3_t end = new_pos;
            KilnTrace tr = kiln_clip_ray(p->pos, end);
            if (tr.fraction < 1.0f) {
                if (p->radius > 0) explode(tr.endpos, p->damage, p->radius);
                else if (g_hit_fn) g_hit_fn(0xFFFF, tr.endpos, p->damage, 0.0f);
                p->active = 0;
                continue;
            }
            for (KilnActor *a = kiln_actor_first(KILN_ACTOR_CAT_ENEMY); a; a = kiln_actor_next(a)) {
                /* Point-to-segment distance: find the closest point on the
                 * segment [p->pos, new_pos] to the enemy, then check if
                 * it's within the hit radius. This prevents tunnelling when
                 * a fast projectile moves more than the hit radius in one
                 * frame — the old point-distance check only tested p->pos. */
                fm_vec3_t seg = {{ new_pos.v[0] - p->pos.v[0],
                                   new_pos.v[1] - p->pos.v[1],
                                   new_pos.v[2] - p->pos.v[2] }};
                float seg_len2 = seg.v[0]*seg.v[0] + seg.v[1]*seg.v[1] + seg.v[2]*seg.v[2];
                fm_vec3_t to_enemy = {{ a->xform.pos.v[0] - p->pos.v[0],
                                        a->xform.pos.v[1] - p->pos.v[1],
                                        a->xform.pos.v[2] - p->pos.v[2] }};
                float t = 0.0f;
                if (seg_len2 > 1e-6f) {
                    t = (to_enemy.v[0]*seg.v[0] + to_enemy.v[1]*seg.v[1] + to_enemy.v[2]*seg.v[2]) / seg_len2;
                    if (t < 0.0f) t = 0.0f;
                    if (t > 1.0f) t = 1.0f;
                }
                fm_vec3_t closest = {{ p->pos.v[0] + seg.v[0] * t,
                                       p->pos.v[1] + seg.v[1] * t,
                                       p->pos.v[2] + seg.v[2] * t }};
                fm_vec3_t d = {{ a->xform.pos.v[0] - closest.v[0],
                                 a->xform.pos.v[1] - closest.v[1],
                                 a->xform.pos.v[2] - closest.v[2] }};
                if (d.v[0]*d.v[0] + d.v[1]*d.v[1] + d.v[2]*d.v[2] < 12.0f * 12.0f) {
                    if (p->radius > 0) explode(p->pos, p->damage, p->radius);
                    else if (g_hit_fn) g_hit_fn(a->profile_id, p->pos, p->damage, 0.0f);
                    p->active = 0;
                    break;
                }
            }
            if (p->active) p->pos = new_pos;
        }
    }
}

void kiln_projectile_draw_all(void)
{
    for (int i = 0; i < KILN_PROJECTILE_MAX; i++) {
        KilnProjectile *p = &g_pool[i];
        if (!p->active) continue;
        T3DVertPacked *v = g_proj_verts[p->type];
        if (!v) continue;
        t3d_vert_load(v, 0, 8);
        for (int j = 0; j < 12; j++)
            t3d_tri_draw(CUBE_TRIS[j][0], CUBE_TRIS[j][1], CUBE_TRIS[j][2]);
        t3d_tri_sync();
    }
}