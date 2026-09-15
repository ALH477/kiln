// SPDX-License-Identifier: MIT
//
// kiln_actor: one flat, caller-owned pool, four categories, handles that go
// stale safely.
//
//   PLAYER  you, on the stick
//   ENEMY   three drones orbiting the arena at different radii and speeds
//   PROP    four glowing pylons that are spawned once and just sit in the list
//   ITEM    gems that spawn at your feet, rise, spin, and despawn THEMSELVES
//           after three seconds — from inside kiln_actor_update_all, which is
//           what the pool's captured next-pointer walk makes safe
//
// The HUD counts each category with kiln_actor_count and gauges the whole pool
// against its 32 slots; the gauge goes red when the pool is nearly full, and a
// refused spawn is counted rather than silently dropped.
//
//   stick walk      idle 2 s: the demo walks a circle
//
// Jump ROM: .#actors-demo-full spawns gems ten times as fast, so the pool fills,
// the gauge goes red and kiln_actor_spawn starts refusing.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_actor.h>
#include <kiln/kiln_prim.h>

enum { JUMP_NONE, JUMP_FULL };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W 320
#define SCREEN_H 240
#define ACTOR_POOL_CAP 32
#define ARENA 100.0f

enum { PROFILE_PLAYER, PROFILE_ENEMY, PROFILE_PROP, PROFILE_ITEM, PROFILE_COUNT };

static KilnPrim g_body, g_nose, g_drone, g_fin_a, g_fin_b, g_pylon_base, g_pylon, g_lamp;
static KilnPrim g_gem, g_gem_cap, g_shadow;
static KilnTransform g_shadow_xf;
static uint32_t g_refused;

// ── Player ──────────────────────────────────────────────────────────────
static void player_init(KilnActor *self, const KilnDict *args)
{
    (void)args;
    self->xform.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
}

static void player_update(KilnActor *self, float dt)
{
    const KilnInput *in = kiln_input_get(1);
    /* Camera looks down +Z: stick up is +Z, stick right is -X. */
    const float dx = -in->stick_x * 90.0f * dt, dz = in->stick_y * 90.0f * dt;
    fm_vec3_t *p = &self->xform.pos;
    p->v[0] += dx; p->v[2] += dz;
    if (p->v[0] >  ARENA) p->v[0] =  ARENA;
    if (p->v[0] < -ARENA) p->v[0] = -ARENA;
    if (p->v[2] >  ARENA) p->v[2] =  ARENA;
    if (p->v[2] < -ARENA) p->v[2] = -ARENA;
    if (dx * dx + dz * dz > 1e-4f) self->xform.rot_angle = fm_atan2f(dx, dz);
}

static void player_draw(KilnActor *self) { (void)self; kiln_prim_draw(&g_body); kiln_prim_draw(&g_nose); }

// ── Enemy: a drone on its own orbit ─────────────────────────────────────
typedef struct { float angle, radius, speed; } EnemyState;

static void enemy_init(KilnActor *self, const KilnDict *args)
{
    (void)args;
    EnemyState *s = (EnemyState *)self->state;
    *s = (EnemyState){ .radius = 40.0f, .speed = 1.0f };
    self->xform.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
}

static void enemy_update(KilnActor *self, float dt)
{
    EnemyState *s = (EnemyState *)self->state;
    s->angle += s->speed * dt;
    self->xform.pos.v[0] = fm_cosf(s->angle) * s->radius;
    self->xform.pos.v[2] = fm_sinf(s->angle) * s->radius;
    self->xform.pos.v[1] = 16.0f + 4.0f * fm_sinf(s->angle * 3.0f);
    self->xform.rot_angle = s->angle * 4.0f;      /* the fins spin */
}

static void enemy_draw(KilnActor *self)
{
    (void)self;
    kiln_prim_draw(&g_drone);
    kiln_prim_draw(&g_fin_a);
    kiln_prim_draw(&g_fin_b);
}

// ── Prop: a pylon ───────────────────────────────────────────────────────
static void prop_draw(KilnActor *self)
{
    (void)self;
    kiln_prim_draw(&g_pylon_base);
    kiln_prim_draw(&g_pylon);
    kiln_prim_draw(&g_lamp);
}

// ── Item: a gem that removes itself ─────────────────────────────────────
typedef struct { float age; } ItemState;

static void item_init(KilnActor *self, const KilnDict *args)
{
    (void)args;
    ((ItemState *)self->state)->age = 0.0f;
    self->xform.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
}

static void item_update(KilnActor *self, float dt)
{
    ItemState *s = (ItemState *)self->state;
    s->age += dt;
    self->xform.pos.v[1] += dt * 18.0f;
    self->xform.rot_angle += dt * 3.0f;
    /* Safe from inside update_all: the walk captured the next actor before
     * calling this (see kiln_actor.c). */
    if (s->age > 3.0f) kiln_actor_despawn(kiln_actor_handle_of(self));
}

static void item_draw(KilnActor *self) { (void)self; kiln_prim_draw(&g_gem); kiln_prim_draw(&g_gem_cap); }

static const KilnActorProfile PROFILES[PROFILE_COUNT] = {
    [PROFILE_PLAYER] = { .name = "player", .category = KILN_ACTOR_CAT_PLAYER,
                         .init = player_init, .update = player_update, .draw = player_draw },
    [PROFILE_ENEMY]  = { .name = "enemy", .category = KILN_ACTOR_CAT_ENEMY,
                         .state_size = sizeof(EnemyState),
                         .init = enemy_init, .update = enemy_update, .draw = enemy_draw },
    [PROFILE_PROP]   = { .name = "prop", .category = KILN_ACTOR_CAT_PROP, .draw = prop_draw },
    [PROFILE_ITEM]   = { .name = "item", .category = KILN_ACTOR_CAT_ITEM,
                         .state_size = sizeof(ItemState),
                         .init = item_init, .update = item_update, .draw = item_draw },
};

static KilnActor g_pool[ACTOR_POOL_CAP];

// ── Tape: a slow circle ─────────────────────────────────────────────────
static const KilnInputKey CIRCLE_KEYS[] = {
    { .frame =   0, .sx =   0, .sy =  80 },
    { .frame =  30, .sx = -56, .sy =  56 },
    { .frame =  60, .sx = -80, .sy =   0 },
    { .frame =  90, .sx = -56, .sy = -56 },
    { .frame = 120, .sx =   0, .sy = -80 },
    { .frame = 150, .sx =  56, .sy = -56 },
    { .frame = 180, .sx =  80, .sy =   0 },
    { .frame = 210, .sx =  56, .sy =  56 },
    { .frame = 240 },
};
static const KilnInputTape CIRCLE = { CIRCLE_KEYS, 9, 0 };

static void chip(int x, int y, color_t c, const char *label, unsigned n)
{
    kiln_gui_rect(x, y - 6, 6, 6, c);
    kiln_gui_text(x + 10, y, RGBA32(0xE8, 0xE8, 0xF0, 0xFF), "%s %2u", label, n);
}

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    kiln_input_init();

    const fm_vec3_t O = {{ 0, 0, 0 }};
    kiln_prim_box(&g_body, (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 7, 9, 7 }},
                  kiln_prim_rgba(0xFF, 0xE0, 0x50), kiln_prim_rgba(0xE0, 0xA0, 0x18), kiln_prim_rgba(0x60, 0x40, 0x00));
    kiln_prim_box(&g_nose, (fm_vec3_t){{ 0, 3, 9 }}, (fm_vec3_t){{ 3, 3, 3 }}, 0xFFFFFFFF, 0xE0E0E8FF, 0x808080FF);
    kiln_prim_box(&g_drone, O, (fm_vec3_t){{ 6, 4, 6 }},
                  kiln_prim_rgba(0xFF, 0x70, 0x90), kiln_prim_rgba(0xD0, 0x40, 0x60), kiln_prim_rgba(0x50, 0x10, 0x20));
    kiln_prim_box(&g_fin_a, O, (fm_vec3_t){{ 14, 1, 3 }},
                  kiln_prim_rgba(0xF0, 0xF0, 0xFF), kiln_prim_rgba(0xA0, 0xA8, 0xC0), kiln_prim_rgba(0x60, 0x60, 0x70));
    kiln_prim_box(&g_fin_b, O, (fm_vec3_t){{ 3, 1, 14 }},
                  kiln_prim_rgba(0xF0, 0xF0, 0xFF), kiln_prim_rgba(0xA0, 0xA8, 0xC0), kiln_prim_rgba(0x60, 0x60, 0x70));
    kiln_prim_box(&g_pylon_base, (fm_vec3_t){{ 0, 3, 0 }}, (fm_vec3_t){{ 9, 3, 9 }},
                  kiln_prim_rgba(0x90, 0x98, 0xA8), kiln_prim_rgba(0x58, 0x60, 0x70), kiln_prim_rgba(0x30, 0x30, 0x38));
    kiln_prim_box(&g_pylon, (fm_vec3_t){{ 0, 22, 0 }}, (fm_vec3_t){{ 4, 16, 4 }},
                  kiln_prim_rgba(0x70, 0x78, 0x88), kiln_prim_rgba(0x48, 0x50, 0x60), kiln_prim_rgba(0x30, 0x30, 0x38));
    kiln_prim_box(&g_lamp, (fm_vec3_t){{ 0, 42, 0 }}, (fm_vec3_t){{ 5, 4, 5 }},
                  kiln_prim_rgba(0x80, 0xFF, 0xF0), kiln_prim_rgba(0x00, 0xF5, 0xD4), kiln_prim_rgba(0x00, 0x80, 0x70));
    kiln_prim_box(&g_gem, O, (fm_vec3_t){{ 4, 4, 4 }},
                  kiln_prim_rgba(0xC0, 0x90, 0xFF), kiln_prim_rgba(0x8B, 0x5C, 0xF6), kiln_prim_rgba(0x40, 0x20, 0x80));
    kiln_prim_box(&g_gem_cap, (fm_vec3_t){{ 0, 6, 0 }}, (fm_vec3_t){{ 2, 2, 2 }},
                  0xFFFFFFFF, kiln_prim_rgba(0xE0, 0xD0, 0xFF), kiln_prim_rgba(0x80, 0x60, 0xC0));
    kiln_prim_floor(&g_shadow, 9.0f, 1, kiln_prim_rgba(0x10, 0x12, 0x1C), kiln_prim_rgba(0x10, 0x12, 0x1C));
    KilnPrim floor_prim;
    kiln_prim_floor(&floor_prim, 130.0f, 13, kiln_prim_rgba(0x34, 0x3A, 0x52), kiln_prim_rgba(0x2A, 0x30, 0x46));
    kiln_transform_init(&g_shadow_xf);
    KilnTransform floor_xf;
    kiln_transform_init(&floor_xf);

    kiln_actor_system_init(PROFILES, PROFILE_COUNT, g_pool, ACTOR_POOL_CAP);
    KilnActorHandle player_h = kiln_actor_spawn(PROFILE_PLAYER, (fm_vec3_t){{ 0, 9, -40 }}, 0.0f, NULL);

    for (int i = 0; i < 3; i++) {
        KilnActor *a = kiln_actor_resolve(kiln_actor_spawn(PROFILE_ENEMY, O, 0.0f, NULL));
        if (!a) continue;
        EnemyState *s = (EnemyState *)a->state;
        s->radius = 34.0f + i * 22.0f;
        s->speed = 0.7f + i * 0.35f;
        s->angle = i * 2.1f;
    }
    static const fm_vec3_t PROP_AT[4] = { {{ 90, 0, 90 }}, {{ -90, 0, 90 }}, {{ 90, 0, -90 }}, {{ -90, 0, -90 }} };
    for (int i = 0; i < 4; i++) kiln_actor_spawn(PROFILE_PROP, PROP_AT[i], 0.0f, NULL);

    KilnScene scene;
    kiln_scene_init(&scene);
    kiln_prim_stage(&scene, RGBA32(0x16, 0x18, 0x2A, 0xFF), 200.0f, 420.0f);
    scene.fov_deg = 60.0f;
    scene.near_z = 10.0f;
    scene.far_z = 420.0f;

    const int spawn_every = KILN_JUMP == JUMP_FULL ? 5 : 50;
    kiln_input_set_attract(1, &CIRCLE, 120);

    uint32_t item_timer = 0, frames = 0;
    float fps = 60.0f;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        kiln_input_update();
        const float dt = 1.0f / 60.0f;
        KilnActor *player = kiln_actor_resolve(player_h);

        /* A gem at the player's feet every `spawn_every` frames. When the pool
         * is full kiln_actor_spawn returns no handle, and that is counted. */
        if (++item_timer >= (uint32_t)spawn_every && player) {
            item_timer = 0;
            fm_vec3_t p = player->xform.pos;
            p.v[1] = 6.0f;
            if (kiln_actor_spawn(PROFILE_ITEM, p, 0.0f, NULL) == KILN_ACTOR_HANDLE_NONE) g_refused++;
        }
        kiln_actor_update_all(dt);

        if (player) {
            const fm_vec3_t pp = player->xform.pos;
            scene.cam_target = (fm_vec3_t){{ pp.v[0] * 0.7f, 10, pp.v[2] * 0.7f + 20 }};
            scene.cam_pos = (fm_vec3_t){{ pp.v[0] * 0.5f, 110, pp.v[2] * 0.5f - 140 }};
        }
        kiln_scene_update(&scene);

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        kiln_frame_begin();
        kiln_scene_begin(&scene);
        kiln_transform_push(&floor_xf); kiln_prim_draw(&floor_prim); kiln_transform_pop();
        for (uint8_t c = KILN_ACTOR_CAT_PLAYER; c <= KILN_ACTOR_CAT_ENEMY; c++) {
            for (KilnActor *a = kiln_actor_first(c); a; a = kiln_actor_next(a)) {
                g_shadow_xf.pos = (fm_vec3_t){{ a->xform.pos.v[0], 0.4f, a->xform.pos.v[2] }};
                kiln_transform_push(&g_shadow_xf); kiln_prim_draw(&g_shadow); kiln_transform_pop();
            }
        }
        kiln_actor_draw_all();

        kiln_gui_begin();
        const color_t teal = RGBA32(0x00, 0xF5, 0xD4, 0xFF);
        const unsigned total = kiln_actor_count(KILN_ACTOR_CATEGORY_COUNT);
        kiln_gui_panel(8, 8, 132, 96, RGBA32(0x0C, 0x10, 0x1C, 0xFF), teal);
        kiln_gui_text(14, 21, teal, "KILN ACTORS");
        chip(14, 35, RGBA32(0xFF, 0xD0, 0x40, 0xFF), "player", kiln_actor_count(KILN_ACTOR_CAT_PLAYER));
        chip(14, 47, RGBA32(0xFF, 0x60, 0x80, 0xFF), "enemy ", kiln_actor_count(KILN_ACTOR_CAT_ENEMY));
        chip(14, 59, teal, "prop  ", kiln_actor_count(KILN_ACTOR_CAT_PROP));
        chip(14, 71, RGBA32(0x8B, 0x5C, 0xF6, 0xFF), "item  ", kiln_actor_count(KILN_ACTOR_CAT_ITEM));
        /* The pool gauge: red once it is nearly out of slots. */
        const int full = total * 10 >= ACTOR_POOL_CAP * 9;
        kiln_gui_text(14, 86, RGBA32(0xE8, 0xE8, 0xF0, 0xFF), "pool %2u/%d", total, ACTOR_POOL_CAP);
        kiln_gui_rect(80, 80, 52, 6, RGBA32(0x30, 0x34, 0x44, 0xFF));
        kiln_gui_rect(80, 80, (int)(52 * total / ACTOR_POOL_CAP), 6,
                      full ? RGBA32(0xFF, 0x40, 0x40, 0xFF) : teal);
        if (g_refused) kiln_gui_text(14, 98, RGBA32(0xFF, 0x60, 0x60, 0xFF), "refused %u", (unsigned)g_refused);
        else kiln_gui_text(14, 98, RGBA32(0x90, 0x98, 0xB0, 0xFF), "%4.1f fps", fps);

        if (kiln_input_scripted(1)) {
            kiln_gui_panel(SCREEN_W - 58, 8, 50, 16, RGBA32(0xC0, 0x30, 0x60, 0xFF), RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
            kiln_gui_text(SCREEN_W - 49, 20, RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "DEMO");
        }
        kiln_gui_panel(8, SCREEN_H - 24, SCREEN_W - 16, 16, RGBA32(0x0C, 0x10, 0x1C, 0xFF),
                       RGBA32(0x8B, 0x5C, 0xF6, 0xFF));
        kiln_gui_text(14, SCREEN_H - 12, RGBA32(0xE8, 0xE8, 0xF0, 0xFF), "stick walk   gems despawn themselves after 3 s");
        kiln_gui_end();
        kiln_frame_end();
    }
}
