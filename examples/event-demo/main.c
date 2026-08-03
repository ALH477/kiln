// SPDX-License-Identifier: MPL-2.0
//
// Phase 4 verification: m64_event. A switch actor posts DOOR_OPEN with a
// 500 ms delay when the player taps A near it; the door actor's event
// callback rotates it open over the next ~700 ms. The HUD reports the
// queued-event count so you can see the 500 ms gap between switch-press
// and door-move start.
//
//   A button      -> m64_event_post(door, DOOR_OPEN, 500 ms)
//   m64_event_process(dt)             -> door's event callback sets target yaw
//   door_update                     -> lerps current yaw toward target yaw
//
// Two actors, one event, one delay. That is the whole shape of id Tech 4's
// idEvent on this hardware: a deferred function call through a queue, not
// a per-actor thread.

#include <libdragon.h>
#include <m64/m64_engine.h>
#include <m64/m64_gui.h>
#include <m64/m64_actor.h>
#include <m64/m64_event.h>
#include <m64/m64_input.h>

#include <malloc.h>

#define SCREEN_W 320
#define SCREEN_H 240
#define ACTOR_POOL_CAP 16

// ── Event ids ───────────────────────────────────────────────────────────
enum {
    EV_DOOR_OPEN = 1,
    EV_DOOR_CLOSE = 2,
};

// ── Profiles ────────────────────────────────────────────────────────────
enum { PROFILE_PLAYER, PROFILE_SWITCH, PROFILE_DOOR, PROFILE_COUNT };

// ── Geometry: a unit cube with per-instance colour ──────────────────────
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

static void draw_cube(T3DVertPacked *verts)
{
    t3d_vert_load(verts, 0, 8);
    for (int i = 0; i < 12; i++)
        t3d_tri_draw(CUBE_TRIS[i][0], CUBE_TRIS[i][1], CUBE_TRIS[i][2]);
    t3d_tri_sync();
}

// ── Static cube buffers ─────────────────────────────────────────────────
static T3DVertPacked *g_cube_player;
static T3DVertPacked *g_cube_switch;
static T3DVertPacked *g_cube_door_off;  /* red  */
static T3DVertPacked *g_cube_door_on;   /* green */

// ── Per-actor state ─────────────────────────────────────────────────────
typedef struct {
    M64ActorHandle door;   /* the door this switch opens            */
    uint8_t pressed;       /* edge: A held this frame?              */
} SwitchState;

typedef struct {
    float cur_yaw;
    float target_yaw;
    int   open;
} DoorState;

// Module-global so the player's update can read the switch handle. Cheaper
// than a per-actor lookup each frame; the demo only has one switch.
static M64ActorHandle g_switch = M64_ACTOR_HANDLE_NONE;

// ── Player ──────────────────────────────────────────────────────────────
static void player_update(M64Actor *self, float dt)
{
    const M64Input *in = m64_input_get(1);
    self->xform.pos.v[0] += in->stick_x * 0.25f * dt * 60.0f;
    self->xform.pos.v[2] -= in->stick_y * 0.25f * dt * 60.0f;
    self->xform.rot_angle += dt;
}
static void player_draw(M64Actor *self) { (void)self; draw_cube(g_cube_player); }

// ── Switch ─────────────────────────────────────────────────────────────
static void switch_init(M64Actor *self, const M64Dict *spawn_args)
{
    (void)spawn_args;
    SwitchState *s = (SwitchState *)self->state;
    s->door = M64_ACTOR_HANDLE_NONE;
    s->pressed = 0;
}

static void switch_update(M64Actor *self, float dt)
{
    (void)dt;
    SwitchState *s = (SwitchState *)self->state;
    const M64Input *in = m64_input_get(1);

    int a_now = (in->buttons & M64_BTN_A) != 0;
    int edge = a_now && !s->pressed;
    s->pressed = a_now;

    if (!edge) return;

    /* A-tap: post DOOR_OPEN to the door with a 500 ms delay. Priority 1 so
     * the pool-full eviction path (which this demo never reaches) would
     * keep player-driven events above ambient ones. */
    if (s->door != M64_ACTOR_HANDLE_NONE) {
        int32_t args[1] = { 1 /* open=1 */ };
        m64_event_post(s->door, EV_DOOR_OPEN, 500, args, 1, 1);
    }
}

static void switch_draw(M64Actor *self) { (void)self; draw_cube(g_cube_switch); }

// ── Door ────────────────────────────────────────────────────────────────
static void door_init(M64Actor *self, const M64Dict *spawn_args)
{
    (void)spawn_args;
    DoorState *s = (DoorState *)self->state;
    s->cur_yaw = 0.0f;
    s->target_yaw = 0.0f;
    s->open = 0;
}

static void door_event(M64Actor *self, uint16_t event_id,
                       const int32_t *args, uint8_t argc)
{
    DoorState *s = (DoorState *)self->state;
    int open = (argc >= 1) ? (int)args[0] : 1;

    if (event_id == EV_DOOR_OPEN) {
        s->open = open;
        s->target_yaw = open ? 1.5708f /* pi/2 */ : 0.0f;
    } else if (event_id == EV_DOOR_CLOSE) {
        s->open = 0;
        s->target_yaw = 0.0f;
    }
}

static void door_update(M64Actor *self, float dt)
{
    DoorState *s = (DoorState *)self->state;
    /* Linear-per-frame damping, same stance as the engine's camera. */
    float t = 4.0f * dt;
    if (t > 1.0f) t = 1.0f;
    s->cur_yaw += (s->target_yaw - s->cur_yaw) * t;
    self->xform.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
    self->xform.rot_angle = s->cur_yaw;
}

static void door_draw(M64Actor *self)
{
    DoorState *s = (DoorState *)self->state;
    /* Tint the door green when its target is "open", red when "closed" so
     * the state is readable from one screenshot, not just the yaw. */
    draw_cube(s->open ? g_cube_door_on : g_cube_door_off);
}

// ── Profile table ───────────────────────────────────────────────────────
static const M64ActorProfile PROFILES[PROFILE_COUNT] = {
    [PROFILE_PLAYER] = { .name = "player", .category = M64_ACTOR_CAT_PLAYER,
                         .state_size = 0,
                         .update = player_update, .draw = player_draw },
    [PROFILE_SWITCH] = { .name = "switch", .category = M64_ACTOR_CAT_PROP,
                         .state_size = sizeof(SwitchState),
                         .init = switch_init, .update = switch_update,
                         .draw = switch_draw },
    [PROFILE_DOOR]   = { .name = "door", .category = M64_ACTOR_CAT_DOOR,
                         .state_size = sizeof(DoorState),
                         .init = door_init, .event = door_event,
                         .update = door_update, .draw = door_draw },
};

static M64Actor g_pool[ACTOR_POOL_CAP];

int main(void)
{
    m64_engine_init(RESOLUTION_320x240);
    joypad_init();
    m64_input_init();

    g_cube_player    = make_color_cube(8,  0xFFD94CFF);
    g_cube_switch    = make_color_cube(6,  0x00F5D4FF);
    g_cube_door_off  = make_color_cube(12, 0xFF4C4CFF);
    g_cube_door_on   = make_color_cube(12, 0x4CFF6AFF);

    m64_actor_system_init(PROFILES, PROFILE_COUNT, g_pool, ACTOR_POOL_CAP);
    m64_event_init();

    /* Spawn order: door first so the switch can capture its handle. */
    M64ActorHandle door_h = m64_actor_spawn(
        PROFILE_DOOR, (fm_vec3_t){{ 30, 0, 0 }}, 0.0f, NULL);
    g_switch = m64_actor_spawn(
        PROFILE_SWITCH, (fm_vec3_t){{ -30, 0, 0 }}, 0.0f, NULL);
    M64Actor *sw = m64_actor_resolve(g_switch);
    if (sw) ((SwitchState *)sw->state)->door = door_h;

    m64_actor_spawn(PROFILE_PLAYER, (fm_vec3_t){{ 0, 0, -60 }}, 0.0f, NULL);

    M64Scene scene;
    m64_scene_init(&scene);
    scene.cam_pos    = (fm_vec3_t){{    0,  80, -120 }};
    scene.cam_target = (fm_vec3_t){{    0,   0,    0 }};
    scene.far_z      = 400.0f;
    scene.ambient[3] = 255;
    m64_scene_update(&scene);

    uint32_t frames = 0;
    float fps = 0.0f;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        m64_input_update();

        float dt = 1.0f / 60.0f;

        /* Events first: any event that fires this frame should land before
         * the actor's own update so the state machine sees it this frame. */
        m64_event_process(dt);
        m64_actor_update_all(dt);

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        /* ── 3D ───────────────────────────────────────────────────── */
        m64_frame_begin();
        m64_scene_begin(&scene);
        m64_actor_draw_all();

        /* ── 2D ───────────────────────────────────────────────────── */
        m64_gui_begin();
        m64_gui_panel(8, 8, 200, 78,
                      RGBA32(10, 10, 24, 200), RGBA32(0, 245, 212, 255));
        m64_gui_text(14, 22, RGBA32(0, 245, 212, 255), "M64 EVENT");
        m64_gui_text(14, 34, RGBA32(232, 232, 240, 255), "fps %5.1f", fps);
        m64_gui_text(14, 46, RGBA32(232, 232, 240, 255),
                     "queued %2u", m64_event_count());
        M64Actor *door = m64_actor_first(M64_ACTOR_CAT_DOOR);
        if (door) {
            DoorState *ds = (DoorState *)door->state;
            m64_gui_text(14, 58, RGBA32(232, 232, 240, 255),
                         "door %s yaw %4.2f",
                         ds->open ? "OPEN " : "CLOSE",
                         ds->cur_yaw);
        }
        m64_gui_text(14, 70, RGBA32(232, 232, 240, 255),
                     "A: open door (500 ms delay)");

        m64_gui_panel(8, SCREEN_H - 28, SCREEN_W - 16, 20,
                      RGBA32(10, 10, 24, 200), RGBA32(139, 92, 246, 255));
        m64_gui_text(14, SCREEN_H - 18, RGBA32(232, 232, 240, 255),
                     "stick: move player   A: trigger switch");

        m64_gui_end();
        m64_frame_end();
    }
}