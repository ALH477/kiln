/* SPDX-License-Identifier: MIT
 *
 * kiln_physics.c — see kiln_physics.h for the model. Built on kiln_clip for
 * world collision; body-vs-body is overlap + positional correction + a
 * 1-axis impulse exchange (the AABB-only simplification the header
 * documents). Single precision throughout, no libm beyond the abs/min
 * that compile to single instructions.
 */

#include "kiln_physics.h"

#include <libdragon.h> /* debugf, assertf */
#include <stddef.h>
#include <string.h>

#define PHYS_FIXED_DT    (1.0f / 60.0f)
#define PHYS_MAX_SUBSTEPS 8

#define PHYS_GRAVITY_DEFAULT (-500.0f)
#define PHYS_SLEEP_VEL2  0.25f   /* |v| < ~0.5 u/s for sleep                 */
#define PHYS_SLEEP_TIME  0.5f    /* ...held for this many seconds            */

#define PHYS_RESTITUTION_DEFAULT 0.2f
#define PHYS_FRICTION_DEFAULT    0.7f

void kiln_physics_init(KilnPhysicsWorld *w, KilnPhysicsBody *bodies, uint16_t cap)
{
    assertf(w, "kiln_physics: world NULL");
    assertf(bodies || cap == 0, "kiln_physics: bodies NULL with cap %u", cap);
    w->bodies    = bodies;
    w->count     = 0;
    w->capacity  = cap;
    w->gravity   = PHYS_GRAVITY_DEFAULT;
    w->fixed_dt  = PHYS_FIXED_DT;
    w->enabled   = 1;
    w->broadphase = 0;
}

void kiln_physics_set_enabled(KilnPhysicsWorld *w, int enabled)
{
    w->enabled = enabled ? 1 : 0;
}

KilnPhysicsBody *kiln_physics_spawn(KilnPhysicsWorld *w, KilnPhysType type,
                                  fm_vec3_t pos, fm_vec3_t half_extents,
                                  float mass)
{
    assertf(w->count < w->capacity,
            "kiln_physics: pool full (%u/%u) — raise capacity at init",
            w->count, w->capacity);
    if (w->count >= w->capacity) return NULL;

    KilnPhysicsBody *b = &w->bodies[w->count++];
    memset(b, 0, sizeof(*b));
    b->pos   = pos;
    b->vel   = (fm_vec3_t){ { 0, 0, 0 } };
    b->mins  = (fm_vec3_t){ { -half_extents.v[0], -half_extents.v[1], -half_extents.v[2] } };
    b->maxs  = (fm_vec3_t){ {  half_extents.v[0],  half_extents.v[1],  half_extents.v[2] } };
    b->type  = (uint8_t)type;
    b->mass  = mass;
    b->inv_mass = (type == KILN_PHYS_DYNAMIC && mass > 0.0f) ? 1.0f / mass : 0.0f;
    b->restitution = PHYS_RESTITUTION_DEFAULT;
    b->friction    = PHYS_FRICTION_DEFAULT;
    b->on_ground = 0;
    b->last_surf = 0;
    b->sleeping  = 0;
    b->sleep_timer = 0.0f;
    return b;
}

void kiln_physics_apply_impulse(KilnPhysicsBody *b, fm_vec3_t imp)
{
    if (!b || b->inv_mass == 0.0f) return;
    b->vel.v[0] += imp.v[0] * b->inv_mass;
    b->vel.v[1] += imp.v[1] * b->inv_mass;
    b->vel.v[2] += imp.v[2] * b->inv_mass;
    /* An impulse wakes a sleeping body — matches HL2's "anything that
     * touches a sleeping prop wakes it". */
    b->sleeping = 0;
    b->sleep_timer = 0.0f;
}

/* ── World collision ────────────────────────────────────────────────────
 *
 * Per dynamic body: integrate gravity, swept-slide against the world via
 * kiln_clip_slide (the same SlideMove shape kiln_player uses), then probe
 * the ground. kiln_clip_slide handles wall-sliding; kiln_clip_ground sets
 * on_ground + last_surf. Vel is scaled by the fraction of the slide that
 * actually moved, so a body blocked against a wall doesn't keep banking
 * horizontal velocity into the wall.
 */
static void integrate_world(KilnPhysicsWorld *w, KilnPhysicsBody *b, float dt)
{
    if (b->type != KILN_PHYS_DYNAMIC) return;
    if (b->sleeping) return;

    /* Gravity. */
    b->vel.v[1] += w->gravity * dt;

    /* Build the per-step displacement and slide. */
    fm_vec3_t disp = {{ b->vel.v[0] * dt, b->vel.v[1] * dt, b->vel.v[2] * dt }};
    fm_vec3_t new_pos = kiln_clip_slide(b->pos, disp, b->mins, b->maxs, 4);

    /* Reconstruct effective velocity from the actual move so a wall hit
     * bleeds off the into-wall component instead of banking it. kiln_clip_slide
     * already clipped velocity internally per iteration; reconstructing here
     * is the cheap way to pick up that effect without exposing slide's
     * internals. */
    fm_vec3_t moved = {{ new_pos.v[0] - b->pos.v[0],
                         new_pos.v[1] - b->pos.v[1],
                         new_pos.v[2] - b->pos.v[2] } };
    if (dt > 1e-5f) {
        b->vel.v[0] = moved.v[0] / dt;
        b->vel.v[1] = moved.v[1] / dt;
        b->vel.v[2] = moved.v[2] / dt;
    }
    b->pos = new_pos;

    /* Ground probe. on_ground drives friction + sleep below. */
    KilnTrace g = kiln_clip_ground(b->pos, b->mins, b->maxs);
    b->on_ground = (g.fraction < 1.0f) ? 1 : 0;
    if (b->on_ground) b->last_surf = g.hitsurface;
}

/* ── Body-vs-body ────────────────────────────────────────────────────────
 *
 * Pairwise O(N²) over the body pool. N is small (≤16 dynamics in the
 * physics-demo, typical OoT prop counts are similar); a body broadphase
 * grid costs more to maintain than it saves at this scale, so the loop is
 * flat. On overlap: positional correction along the minimum-translation
 * axis (the axis with the smallest penetration), then a 1-axis impulse
 * exchange along that axis with restitution + friction.
 *
 * Statics (inv_mass = 0) absorb; kinematics push but aren't pushed. This
 * is the AABB-only simplification — a real VPhysics would do OBB contact
 * manifolds with multiple contact points; here we get one axis per pair,
 * which is enough to stack crates stably (the contact axis is Y, the
 * impulse exchanges vertical velocity, friction kills the rest).
 */
static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }

static int aabb_overlap(const KilnPhysicsBody *a, const KilnPhysicsBody *b,
                        float *pen, int *axis)
{
    /* Overlap on each axis; the minimum-overlap axis is the contact axis. */
    float ax0 = b->pos.v[0] + b->mins.v[0] - (a->pos.v[0] + a->maxs.v[0]);
    float ax1 = a->pos.v[0] + a->mins.v[0] - (b->pos.v[0] + b->maxs.v[0]);
    float ay0 = b->pos.v[1] + b->mins.v[1] - (a->pos.v[1] + a->maxs.v[1]);
    float ay1 = a->pos.v[1] + a->mins.v[1] - (b->pos.v[1] + b->maxs.v[1]);
    float az0 = b->pos.v[2] + b->mins.v[2] - (a->pos.v[2] + a->maxs.v[2]);
    float az1 = a->pos.v[2] + a->mins.v[2] - (b->pos.v[2] + b->maxs.v[2]);

    /* If any axis has a gap, no overlap. ax0 > 0 means b's min is past a's
     * max → separating gap on X. */
    if (ax0 > 0.0f || ay0 > 0.0f || az0 > 0.0f) return 0;
    if (ax1 > 0.0f || ay1 > 0.0f || az1 > 0.0f) return 0;

    /* Penetration depths (positive). The minimum is the contact axis. */
    float px = -maxf(ax0, ax1);
    float py = -maxf(ay0, ay1);
    float pz = -maxf(az0, az1);
    float minp = px; int mina = 0;
    if (py < minp) { minp = py; mina = 1; }
    if (pz < minp) { minp = pz; mina = 2; }
    *pen = minp;
    *axis = mina;
    return 1;
}

static void resolve_pair(KilnPhysicsBody *a, KilnPhysicsBody *b,
                         float pen, int axis)
{
    /* Wake sleeping bodies on contact. */
    if (a->sleeping) { a->sleeping = 0; a->sleep_timer = 0.0f; }
    if (b->sleeping) { b->sleeping = 0; b->sleep_timer = 0.0f; }

    /* Positional correction: split the penetration by inverse mass.
     * Direction: from a to b along the contact axis, sign chosen so b is
     * pushed in +axis if b's center is greater on that axis. */
    float sign = (b->pos.v[axis] >= a->pos.v[axis]) ? 1.0f : -1.0f;
    float ima = a->inv_mass, imb = b->inv_mass;
    float total_inv = ima + imb;
    if (total_inv <= 0.0f) return; /* two statics/kinematics — nothing to do */

    /* Slop: don't bother correcting < 1mm penetrations (CLIP_EPS-scale). */
    if (pen > 1e-3f) {
        float corr = (pen - 1e-3f) / total_inv;
        a->pos.v[axis] -= sign * corr * ima;
        b->pos.v[axis] += sign * corr * imb;
    }

    /* Impulse along the contact axis. Relative velocity into-contact. */
    float va = a->vel.v[axis], vb = b->vel.v[axis];
    float rel = (vb - va) * sign; /* > 0 means approaching */
    if (rel <= 0.0f) return;      /* separating, no impulse needed */

    float e = (a->restitution < b->restitution) ? a->restitution : b->restitution;
    float j = -(1.0f + e) * rel / total_inv;
    a->vel.v[axis] -= sign * j * ima;
    b->vel.v[axis] += sign * j * imb;

    /* Friction: dampen the two tangential axes proportionally to the
     * normal impulse, capped by the body friction coefficients. Cheap
     * approximation of Coulomb friction — enough to keep stacks from
     * sliding out from under each other. */
    float f = (a->friction < b->friction) ? a->friction : b->friction;
    if (f <= 0.0f) return;
    int t1 = (axis == 0) ? 1 : 0;
    int t2 = (axis == 2) ? 1 : 2;
    for (int t = 0; t < 2; t++) {
        int ax = (t == 0) ? t1 : t2;
        float vt = (b->vel.v[ax] - a->vel.v[ax]) * sign;
        float jt = -vt * f / total_inv;
        a->vel.v[ax] -= sign * jt * ima;
        b->vel.v[ax] += sign * jt * imb;
    }
}

static void resolve_body_body(KilnPhysicsWorld *w)
{
    for (uint16_t i = 0; i < w->count; i++) {
        KilnPhysicsBody *a = &w->bodies[i];
        if (a->type == KILN_PHYS_STATIC) continue; /* statics only react */
        for (uint16_t k = i + 1; k < w->count; k++) {
            KilnPhysicsBody *b = &w->bodies[k];
            if (b->type == KILN_PHYS_STATIC && a->type != KILN_PHYS_DYNAMIC)
                continue; /* kinematic-vs-static: skip */
            float pen; int axis;
            if (!aabb_overlap(a, b, &pen, &axis)) continue;
            /* Kinematic-vs-kinematic or static-vs-static: no resolution. */
            if (a->inv_mass == 0.0f && b->inv_mass == 0.0f) continue;
            resolve_pair(a, b, pen, axis);
        }
    }
}

/* ── Friction & sleep ────────────────────────────────────────────────────
 *
 * Grounded bodies get horizontal friction (exponential decay — one mul per
 * axis per frame). Sleep accrues when |vel|² is below the threshold; a
 * body that sleeps is skipped by integrate_world until a contact or
 * impulse wakes it. Without sleeping, a stack of crates keeps doing tiny
 * gravity-compensation bounces forever and never settles.
 */
static void friction_and_sleep(KilnPhysicsWorld *w, float dt)
{
    for (uint16_t i = 0; i < w->count; i++) {
        KilnPhysicsBody *b = &w->bodies[i];
        if (b->type != KILN_PHYS_DYNAMIC) continue;
        if (b->sleeping) continue;

        if (b->on_ground) {
            /* Horizontal friction. Decay factor tuned for 60 Hz; clamp the
             * dt-dependent exponent to avoid overshoot on a stutter frame. */
            float k = 1.0f - b->friction * minf(dt * 60.0f, 1.0f);
            b->vel.v[0] *= k;
            b->vel.v[2] *= k;
            /* Vertical velocity killed when grounded (resting contact). */
            if (b->vel.v[1] < 0.0f) b->vel.v[1] = 0.0f;
        }

        float v2 = b->vel.v[0]*b->vel.v[0]
                 + b->vel.v[1]*b->vel.v[1]
                 + b->vel.v[2]*b->vel.v[2];
        if (b->on_ground && v2 < PHYS_SLEEP_VEL2) {
            b->sleep_timer += dt;
            if (b->sleep_timer >= PHYS_SLEEP_TIME) {
                b->sleeping = 1;
                b->vel = (fm_vec3_t){ { 0, 0, 0 } };
            }
        } else {
            b->sleep_timer = 0.0f;
        }
    }
}

void kiln_physics_step(KilnPhysicsWorld *w, float dt)
{
    if (!w->enabled) return;
    if (w->count == 0) return;

    /* Slice dt into fixed substeps for stable integration. Cap the count
     * so a long frame (e.g. the first frame after init) doesn't spiral. */
    int n = (int)(dt / w->fixed_dt + 0.5f);
    if (n < 1) n = 1;
    if (n > PHYS_MAX_SUBSTEPS) n = PHYS_MAX_SUBSTEPS;
    float h = dt / (float)n;

    for (int s = 0; s < n; s++) {
        for (uint16_t i = 0; i < w->count; i++) {
            integrate_world(w, &w->bodies[i], h);
        }
        resolve_body_body(w);
        friction_and_sleep(w, h);
    }
}