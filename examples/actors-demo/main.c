// SPDX-License-Identifier: MIT
//
// Phase B verification: the actor system (kiln_actor.h) with one actor type
// per category that matters for this demo — PLAYER, ENEMY, PROP, ITEM — to
// exercise spawn, despawn-by-handle, despawn-of-self-during-update, the
// fixed category draw order, and category-scoped counts via kiln_actor_count.
//
// Geometry is a handful of hand-built colour-tinted cubes, same reasoning as
// examples/engine: this ROM is about the actor system, not the asset
// pipeline (that's examples/assets-demo).

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_actor.h>

#include <malloc.h>

#define SCREEN_W 320
#define SCREEN_H 240
#define ACTOR_POOL_CAP 32

// The 12 triangles of a cube, as index triples into 8 corners — shared by
// every colour variant below.
static const uint8_t CUBE_TRIS[12][3] = {
    {0,1,2},{2,3,0}, {4,6,5},{6,4,7},
    {0,4,5},{5,1,0}, {1,5,6},{6,2,1},
    {2,6,7},{7,3,2}, {3,7,4},{4,0,3},
};

static T3DVertPacked *make_color_cube(int16_t half, uint32_t rgba)
{
    T3DVertPacked *v = malloc_uncached(sizeof(T3DVertPacked) * 4);
    const int16_t s = half;
    const int16_t c[8][3] = {
        {-s,-s,-s},{ s,-s,-s},{ s, s,-s},{-s, s,-s},
        {-s,-s, s},{ s,-s, s},{ s, s, s},{-s, s, s},
    };
    for (int i = 0; i < 8; i += 2) {
        fm_vec3_t na = {{ (float)c[i][0], (float)c[i][1], (float)c[i][2] }};
        fm_vec3_t nb = {{ (float)c[i+1][0], (float)c[i+1][1], (float)c[i+1][2] }};
        fm_vec3_norm(&na, &na);
        fm_vec3_norm(&nb, &nb);
        v[i / 2] = (T3DVertPacked){
            .posA = { c[i][0], c[i][1], c[i][2] }, .rgbaA = rgba,
            .normA = t3d_vert_pack_normal(&na),
            .posB = { c[i+1][0], c[i+1][1], c[i+1][2] }, .rgbaB = rgba,
            .normB = t3d_vert_pack_normal(&nb),
        };
    }
    return v;
}

static void draw_cube(T3DVertPacked *verts)
{
    t3d_vert_load(verts, 0, 8);
    for (int i = 0; i < 12; i++) t3d_tri_draw(CUBE_TRIS[i][0], CUBE_TRIS[i][1], CUBE_TRIS[i][2]);
    t3d_tri_sync();
}

// ── Actor profiles ─────────────────────────────────────────────────────

enum { PROFILE_PLAYER, PROFILE_ENEMY, PROFILE_PROP, PROFILE_ITEM, PROFILE_COUNT };

static T3DVertPacked *g_cube_player;
static T3DVertPacked *g_cube_enemy;
static T3DVertPacked *g_cube_prop;
static T3DVertPacked *g_cube_item;

static joypad_inputs_t g_input;

static void player_update(KilnActor *self, float dt)
{
    self->xform.pos.v[0] += (float)g_input.stick_x * 0.25f * dt * 60.0f;
    self->xform.pos.v[2] -= (float)g_input.stick_y * 0.25f * dt * 60.0f;
    self->xform.rot_angle += dt;
}
static void player_draw(KilnActor *self) { (void)self; draw_cube(g_cube_player); }

typedef struct { float angle, radius, speed; } EnemyState;

static void enemy_init(KilnActor *self, const KilnDict *spawn_args)
{
    (void)spawn_args;
    EnemyState *s = (EnemyState *)self->state;
    s->angle = 0.0f;
    s->radius = 40.0f;
    s->speed = 1.0f;
}
static void enemy_update(KilnActor *self, float dt)
{
    EnemyState *s = (EnemyState *)self->state;
    s->angle += s->speed * dt;
    self->xform.pos.v[0] = fm_cosf(s->angle) * s->radius;
    self->xform.pos.v[2] = fm_sinf(s->angle) * s->radius;
    self->xform.rot_angle = s->angle;
}
static void enemy_draw(KilnActor *self) { (void)self; draw_cube(g_cube_enemy); }

static void prop_draw(KilnActor *self) { (void)self; draw_cube(g_cube_prop); }

typedef struct { float age; } ItemState;

static void item_init(KilnActor *self, const KilnDict *spawn_args)
{
    (void)spawn_args;
    ((ItemState *)self->state)->age = 0.0f;
}
static void item_update(KilnActor *self, float dt)
{
    ItemState *s = (ItemState *)self->state;
    s->age += dt;
    self->xform.pos.v[1] += dt * 20.0f;
    self->xform.rot_angle += dt * 3.0f;
    // Items despawn themselves once their lifetime expires — safe from
    // inside update_all because the walk already captured the next pointer
    // before calling this function (see kiln_actor.c).
    if (s->age > 3.0f) kiln_actor_despawn(kiln_actor_handle_of(self));
}
static void item_draw(KilnActor *self) { (void)self; draw_cube(g_cube_item); }

static const KilnActorProfile PROFILES[PROFILE_COUNT] = {
    [PROFILE_PLAYER] = { .name = "player", .category = KILN_ACTOR_CAT_PLAYER,
                          .state_size = 0, .update = player_update, .draw = player_draw },
    [PROFILE_ENEMY]  = { .name = "enemy", .category = KILN_ACTOR_CAT_ENEMY,
                          .state_size = sizeof(EnemyState),
                          .init = enemy_init, .update = enemy_update, .draw = enemy_draw },
    [PROFILE_PROP]   = { .name = "prop", .category = KILN_ACTOR_CAT_PROP,
                          .state_size = 0, .draw = prop_draw },
    [PROFILE_ITEM]   = { .name = "item", .category = KILN_ACTOR_CAT_ITEM,
                          .state_size = sizeof(ItemState),
                          .init = item_init, .update = item_update, .draw = item_draw },
};

static KilnActor g_pool[ACTOR_POOL_CAP];

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();

    g_cube_player = make_color_cube(8, 0xFFD94CFF);
    g_cube_enemy  = make_color_cube(10, 0xFF4C6AFF);
    g_cube_prop   = make_color_cube(14, 0x00F5D4FF);
    g_cube_item   = make_color_cube(5, 0x8B5CF6FF);

    kiln_actor_system_init(PROFILES, PROFILE_COUNT, g_pool, ACTOR_POOL_CAP);

    kiln_actor_spawn(PROFILE_PLAYER, (fm_vec3_t){{ 0, 0, 0 }}, 0.0f, NULL);

    // Three orbiting enemies at different radii/speeds, and four static
    // corner props — spawned once, never despawned, to show a category that
    // just sits in the draw list.
    for (int i = 0; i < 3; i++) {
        KilnActorHandle h = kiln_actor_spawn(PROFILE_ENEMY, (fm_vec3_t){{ 0, 0, 0 }}, 0.0f, NULL);
        KilnActor *a = kiln_actor_resolve(h);
        EnemyState *s = (EnemyState *)a->state;
        s->radius = 30.0f + i * 15.0f;
        s->speed = 0.6f + i * 0.3f;
        s->angle = i * 2.1f;
    }
    static const fm_vec3_t prop_pos[4] = {
        {{ 60, 0, 60 }}, {{ -60, 0, 60 }}, {{ 60, 0, -60 }}, {{ -60, 0, -60 }},
    };
    for (int i = 0; i < 4; i++) kiln_actor_spawn(PROFILE_PROP, prop_pos[i], 0.0f, NULL);

    KilnScene scene;
    kiln_scene_init(&scene);
    scene.far_z = 300.0f;

    uint32_t item_timer = 0;
    uint32_t frames = 0;
    float fps = 0.0f;
    uint32_t last_ticks = get_ticks();
    float t = 0.0f;

    for (;;) {
        joypad_poll();
        g_input = joypad_get_inputs(JOYPAD_PORT_1);

        // A new item every ~50 frames (~0.83s at 60fps); each despawns
        // itself after 3s, so the item count settles into a steady state
        // rather than growing until the pool fills.
        if (++item_timer >= 50) {
            item_timer = 0;
            fm_vec3_t pos = {{ fm_sinf(t) * 20.0f, 0, fm_cosf(t) * 20.0f }};
            kiln_actor_spawn(PROFILE_ITEM, pos, 0.0f, NULL);
        }

        float dt = 1.0f / 60.0f;
        t += dt;
        kiln_actor_update_all(dt);

        KilnActor *player = kiln_actor_first(KILN_ACTOR_CAT_PLAYER);
        if (player) {
            scene.cam_target = player->xform.pos;
            scene.cam_pos = (fm_vec3_t){{
                player->xform.pos.v[0],
                player->xform.pos.v[1] + 60.0f,
                player->xform.pos.v[2] - 110.0f,
            }};
        }
        kiln_scene_update(&scene);

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        kiln_frame_begin();
        kiln_scene_begin(&scene);
        kiln_actor_draw_all();

        kiln_gui_begin();
        kiln_gui_panel(8, 8, 160, 70,
                      RGBA32(10, 10, 24, 200), RGBA32(0, 245, 212, 255));
        kiln_gui_text(14, 22, RGBA32(0, 245, 212, 255), "KILN ACTORS");
        kiln_gui_text(14, 34, RGBA32(232, 232, 240, 255), "fps %5.1f", fps);
        kiln_gui_text(14, 46, RGBA32(232, 232, 240, 255), "enemy %2u  prop %2u",
                     kiln_actor_count(KILN_ACTOR_CAT_ENEMY), kiln_actor_count(KILN_ACTOR_CAT_PROP));
        kiln_gui_text(14, 58, RGBA32(232, 232, 240, 255), "item %2u  total %2u",
                     kiln_actor_count(KILN_ACTOR_CAT_ITEM), kiln_actor_count(KILN_ACTOR_CATEGORY_COUNT));
        kiln_gui_text(14, 70, RGBA32(232, 232, 240, 255), "stick: move player");
        kiln_gui_end();

        kiln_frame_end();
    }
}
