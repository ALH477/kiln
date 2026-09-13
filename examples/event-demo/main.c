// SPDX-License-Identifier: MIT
//
// kiln_event: a switch, a door, and the half second between them.
//
// Stand at the pedestal and press A. The switch posts DOOR_OPEN to the door
// with a 500 ms delay; the door swings open on its hinge when the event lands,
// and posts DOOR_CLOSE to itself four seconds out. If you are standing in the
// doorway when that fires, the door re-posts the close instead of shutting on
// you. Every event is a deferred call through one flat queue — id Tech 4's
// idEvent on this hardware, not a per-actor thread.
//
//   kiln_event   -> post with delay, process before actor update, chained posts
//   kiln_clip    -> the closed door is a brush; opening removes it
//   kiln_prim    -> a door slab built with its hinge at the origin, so rotating
//                   the transform swings it instead of spinning it in place
//
//   stick walk   A press the switch (stand at the pedestal)
//   idle 2 s: the demo presses it, walks through, and gets caught in the door
//
// Jumps: .#event-demo-open boots with the door open and held; .#event-demo-queued
// boots with DOOR_OPEN posted a minute out, so the pending queue is on screen.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_actor.h>
#include <kiln/kiln_event.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_clip.h>
#include <kiln/kiln_audio.h>
#include <kiln/kiln_prim.h>
#include <kiln/kiln_debugdraw.h>

enum { JUMP_NONE, JUMP_OPEN, JUMP_QUEUED };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W 320
#define SCREEN_H 240
#define ACTOR_POOL_CAP 8
#define WALK_SPEED 90.0f
#define SWITCH_RANGE 26.0f
#define DOOR_HALF_W 14.0f
#define WALL_Z 40.0f

enum { EV_DOOR_OPEN = 1, EV_DOOR_CLOSE = 2 };
enum { PROFILE_SWITCH, PROFILE_DOOR, PROFILE_COUNT };

static const fm_vec3_t P_MINS = {{ -7, -10, -7 }};
static const fm_vec3_t P_MAXS = {{  7,  10,  7 }};
static const fm_vec3_t SWITCH_AT = {{ -56, 0, -6 }};

// ── The queue, mirrored for the HUD ─────────────────────────────────────
// kiln_event exposes a count, not its slots — correctly, since a caller that
// could read the pool could come to depend on its layout. The demo keeps its
// own note of what it posted so the countdown can be drawn.
typedef struct { const char *label; float left, total; } Pending;
static Pending g_pending[4];

static void note_post(const char *label, int delay_ms)
{
    for (int i = 0; i < 4; i++) {
        if (g_pending[i].left <= 0.0f) {
            g_pending[i] = (Pending){ label, delay_ms / 1000.0f, delay_ms / 1000.0f };
            return;
        }
    }
}

// ── World ───────────────────────────────────────────────────────────────
// A wall across z = WALL_Z with a doorway, plus the door itself as the last
// brush — present while the door is shut, dropped from the count when open.
static KilnBrush g_world[] = {
    { .mins = {{ -120, 0, WALL_Z }},        .maxs = {{ -DOOR_HALF_W, 44, WALL_Z + 8 }} },
    { .mins = {{  DOOR_HALF_W, 0, WALL_Z }}, .maxs = {{ 120, 44, WALL_Z + 8 }} },
    { .mins = {{ -DOOR_HALF_W, 36, WALL_Z }}, .maxs = {{ DOOR_HALF_W, 44, WALL_Z + 8 }} },
    { .mins = {{ -120, 0, -90 }}, .maxs = {{ -112, 44, WALL_Z }} },
    { .mins = {{  112, 0, -90 }}, .maxs = {{  120, 44, WALL_Z }} },
    { .mins = {{ -DOOR_HALF_W, 0, WALL_Z + 2 }}, .maxs = {{ DOOR_HALF_W, 36, WALL_Z + 6 }} }, /* door */
};
#define WALL_BRUSHES 5
static int g_door_blocks = 1;

static KilnPrim g_wall[WALL_BRUSHES], g_floor, g_floor_far, g_door, g_body, g_nose, g_shadow;
static KilnPrim g_pedestal, g_button_up, g_button_down, g_chest, g_chest_lid;
static int g_door_open_sfx = -1, g_click_sfx = -1;
static fm_vec3_t g_player = {{ 0, 11, -60 }};

// ── Switch ──────────────────────────────────────────────────────────────
typedef struct { KilnActorHandle door; float pressed_t; } SwitchState;

static void switch_update(KilnActor *self, float dt)
{
    SwitchState *s = (SwitchState *)self->state;
    if (s->pressed_t > 0) s->pressed_t -= dt;
    if (!kiln_input_pressed(1, KILN_BTN_A)) return;

    /* Only from the pedestal. The old demo accepted A from anywhere. */
    const float dx = g_player.v[0] - self->xform.pos.v[0], dz = g_player.v[2] - self->xform.pos.v[2];
    if (dx * dx + dz * dz > SWITCH_RANGE * SWITCH_RANGE) return;

    s->pressed_t = 0.4f;
    kiln_sfx_play(g_click_sfx, -1, 1);
    int32_t args[1] = { 1 };
    if (kiln_event_post(s->door, EV_DOOR_OPEN, 500, args, 1, 1) == 0) note_post("DOOR_OPEN", 500);
}

static void switch_draw(KilnActor *self)
{
    const SwitchState *s = (const SwitchState *)self->state;
    kiln_prim_draw(&g_pedestal);
    kiln_prim_draw(s->pressed_t > 0 ? &g_button_down : &g_button_up);
}

// ── Door ────────────────────────────────────────────────────────────────
typedef struct { float cur, target; int open, hold_open; } DoorState;

static int player_in_doorway(void)
{
    return g_player.v[0] + P_MAXS.v[0] > -DOOR_HALF_W - 2 && g_player.v[0] + P_MINS.v[0] < DOOR_HALF_W + 2 &&
           g_player.v[2] + P_MAXS.v[2] > WALL_Z - 30 && g_player.v[2] + P_MINS.v[2] < WALL_Z + 30;
}

static void door_event(KilnActor *self, uint16_t event_id, const int32_t *args, uint8_t argc)
{
    (void)args; (void)argc;
    DoorState *s = (DoorState *)self->state;
    if (event_id == EV_DOOR_OPEN && !s->open) {
        s->open = 1;
        s->target = -1.5708f;
        g_door_blocks = 0;
        kiln_sfx_play(g_door_open_sfx, -1, 1);
        /* Chain: the door schedules its own close. */
        if (!s->hold_open && kiln_event_post(kiln_actor_handle_of(self), EV_DOOR_CLOSE, 4000, NULL, 0, 0) == 0)
            note_post("DOOR_CLOSE", 4000);
    } else if (event_id == EV_DOOR_CLOSE && s->open) {
        if (player_in_doorway()) {
            /* Somebody is in the way: ask again shortly rather than closing
             * the collision brush on top of them. */
            if (kiln_event_post(kiln_actor_handle_of(self), EV_DOOR_CLOSE, 700, NULL, 0, 0) == 0)
                note_post("CLOSE (retry)", 700);
            return;
        }
        s->open = 0;
        s->target = 0.0f;
        g_door_blocks = 1;
        kiln_sfx_play(g_click_sfx, -1, 1);
    }
}

static void door_update(KilnActor *self, float dt)
{
    DoorState *s = (DoorState *)self->state;
    float t = 5.0f * dt;
    if (t > 1.0f) t = 1.0f;
    s->cur += (s->target - s->cur) * t;
    self->xform.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
    self->xform.rot_angle = s->cur;
}

static void door_draw(KilnActor *self) { (void)self; kiln_prim_draw(&g_door); }

static const KilnActorProfile PROFILES[PROFILE_COUNT] = {
    [PROFILE_SWITCH] = { .name = "switch", .category = KILN_ACTOR_CAT_PROP,
                         .state_size = sizeof(SwitchState), .update = switch_update, .draw = switch_draw },
    [PROFILE_DOOR]   = { .name = "door", .category = KILN_ACTOR_CAT_DOOR,
                         .state_size = sizeof(DoorState), .event = door_event,
                         .update = door_update, .draw = door_draw },
};
static KilnActor g_pool[ACTOR_POOL_CAP];

// ── Tape ────────────────────────────────────────────────────────────────
// Camera looks down +Z: stick up is +Z, stick right is -X.
static const KilnInputKey ATTRACT_KEYS[] = {
    { .frame =   0, .sx =  64, .sy =  60 },   // to the pedestal
    { .frame =  52 },
    { .frame =  64, .buttons = KILN_BTN_A },
    { .frame =  70 },
    { .frame = 110, .sx = -64, .sy =  62 },   // to the doorway
    { .frame = 162, .sy =  85 },              // through it
    { .frame = 196 },
    { .frame = 300, .sy = -85 },              // back into the doorway as it closes
    { .frame = 326 },
    { .frame = 380, .sy = -85 },
    { .frame = 420, .sx =  40, .sy = -40 },
    { .frame = 470 },
    { .frame = 560 },
};
static const KilnInputTape ATTRACT = { ATTRACT_KEYS, 13, 0 };

static fm_vec3_t centre(const KilnBrush *b)
{
    return (fm_vec3_t){{ (b->mins.v[0] + b->maxs.v[0]) * 0.5f, (b->mins.v[1] + b->maxs.v[1]) * 0.5f,
                         (b->mins.v[2] + b->maxs.v[2]) * 0.5f }};
}
static fm_vec3_t half(const KilnBrush *b)
{
    return (fm_vec3_t){{ (b->maxs.v[0] - b->mins.v[0]) * 0.5f, (b->maxs.v[1] - b->mins.v[1]) * 0.5f,
                         (b->maxs.v[2] - b->mins.v[2]) * 0.5f }};
}

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    /* Mount the ROM's DragonFS before anything opens rom:/. The host resolves
     * rom:/ paths without it, so host renders never noticed; on console the
     * first kiln_sfx_load asserted "File not found". */
    dfs_init(DFS_DEFAULT_LOCATION);
    kiln_input_init();
    kiln_audio_init(KILN_AUDIO_DEFAULT);
    g_door_open_sfx = kiln_sfx_load("rom:/sfx/door_open.wav64");
    g_click_sfx = kiln_sfx_load("rom:/sfx/blip.wav64");

    for (int i = 0; i < WALL_BRUSHES; i++)
        kiln_prim_box(&g_wall[i], centre(&g_world[i]), half(&g_world[i]),
                      kiln_prim_rgba(0xD8, 0xC8, 0xA8), kiln_prim_rgba(0x9C, 0x84, 0x68),
                      kiln_prim_rgba(0x40, 0x34, 0x28));
    kiln_prim_floor(&g_floor, 120.0f, 12, kiln_prim_rgba(0x78, 0x70, 0x60), kiln_prim_rgba(0x6A, 0x62, 0x54));
    kiln_prim_floor(&g_floor_far, 120.0f, 8, kiln_prim_rgba(0x48, 0x70, 0x50), kiln_prim_rgba(0x40, 0x64, 0x48));
    /* The door slab with its hinge on the origin: centre offset by its own
     * half-width, so rotating about Y swings it from the -X jamb. */
    kiln_prim_box(&g_door, (fm_vec3_t){{ DOOR_HALF_W, 18, 0 }}, (fm_vec3_t){{ DOOR_HALF_W, 18, 2 }},
                  kiln_prim_rgba(0xB0, 0x70, 0x38), kiln_prim_rgba(0x88, 0x50, 0x28), kiln_prim_rgba(0x50, 0x30, 0x18));
    kiln_prim_box(&g_body, (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 7, 10, 7 }},
                  kiln_prim_rgba(0xFF, 0xE0, 0x50), kiln_prim_rgba(0xE0, 0xA0, 0x18), kiln_prim_rgba(0x60, 0x40, 0x00));
    kiln_prim_box(&g_nose, (fm_vec3_t){{ 0, 3, 9 }}, (fm_vec3_t){{ 3, 3, 3 }},
                  0xFFFFFFFF, 0xE0E0E8FF, 0x808080FF);
    kiln_prim_floor(&g_shadow, 10.0f, 1, kiln_prim_rgba(0x20, 0x1C, 0x18), kiln_prim_rgba(0x20, 0x1C, 0x18));
    kiln_prim_box(&g_pedestal, (fm_vec3_t){{ 0, 8, 0 }}, (fm_vec3_t){{ 8, 8, 8 }},
                  kiln_prim_rgba(0xC0, 0xC4, 0xD0), kiln_prim_rgba(0x80, 0x84, 0x94), kiln_prim_rgba(0x40, 0x40, 0x48));
    kiln_prim_box(&g_button_up, (fm_vec3_t){{ 0, 19, 0 }}, (fm_vec3_t){{ 4, 3, 4 }},
                  kiln_prim_rgba(0xFF, 0x40, 0x60), kiln_prim_rgba(0xC0, 0x20, 0x40), kiln_prim_rgba(0x60, 0x10, 0x20));
    kiln_prim_box(&g_button_down, (fm_vec3_t){{ 0, 17, 0 }}, (fm_vec3_t){{ 4, 1, 4 }},
                  kiln_prim_rgba(0x60, 0xFF, 0x90), kiln_prim_rgba(0x30, 0xC0, 0x60), kiln_prim_rgba(0x10, 0x60, 0x30));
    kiln_prim_box(&g_chest, (fm_vec3_t){{ 0, 7, 110 }}, (fm_vec3_t){{ 12, 7, 8 }},
                  kiln_prim_rgba(0xA0, 0x60, 0x30), kiln_prim_rgba(0x80, 0x48, 0x20), kiln_prim_rgba(0x40, 0x24, 0x10));
    kiln_prim_box(&g_chest_lid, (fm_vec3_t){{ 0, 16, 110 }}, (fm_vec3_t){{ 13, 2, 9 }},
                  kiln_prim_rgba(0xFF, 0xD0, 0x40), kiln_prim_rgba(0xD0, 0xA0, 0x20), kiln_prim_rgba(0x80, 0x60, 0x10));

    kiln_actor_system_init(PROFILES, PROFILE_COUNT, g_pool, ACTOR_POOL_CAP);
    kiln_event_init();

    /* Door first so the switch can hold its handle. */
    KilnActorHandle door_h = kiln_actor_spawn(PROFILE_DOOR,
        (fm_vec3_t){{ -DOOR_HALF_W, 0, WALL_Z + 4 }}, 0.0f, NULL);
    KilnActorHandle sw_h = kiln_actor_spawn(PROFILE_SWITCH, SWITCH_AT, 0.0f, NULL);
    KilnActor *sw = kiln_actor_resolve(sw_h);
    if (sw) ((SwitchState *)sw->state)->door = door_h;

    if (KILN_JUMP == JUMP_OPEN) {
        KilnActor *d = kiln_actor_resolve(door_h);
        if (d) ((DoorState *)d->state)->hold_open = 1;
        kiln_event_post(door_h, EV_DOOR_OPEN, 0, NULL, 0, 1);
        g_player = (fm_vec3_t){{ 0, 11, 0 }};
    } else if (KILN_JUMP == JUMP_QUEUED) {
        kiln_event_post(door_h, EV_DOOR_OPEN, 60000, NULL, 0, 1);
        note_post("DOOR_OPEN", 60000);
        g_player = (fm_vec3_t){{ -40, 11, -20 }};
    } else {
        kiln_input_set_attract(1, &ATTRACT, 120);
    }

    KilnScene scene;
    kiln_scene_init(&scene);
    kiln_prim_stage(&scene, RGBA32(0x40, 0x5C, 0x7C, 0xFF), 260.0f, 520.0f);
    scene.fov_deg = 60.0f;
    scene.near_z = 12.0f;
    scene.far_z = 520.0f;

    KilnTransform xf_floor, xf_far, xf_body, xf_shadow;
    kiln_transform_init(&xf_floor);
    kiln_transform_init(&xf_far);
    kiln_transform_init(&xf_body);
    kiln_transform_init(&xf_shadow);
    xf_floor.pos = (fm_vec3_t){{ 0, 0, -40 }};
    xf_far.pos = (fm_vec3_t){{ 0, 0, 160 }};
    xf_body.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};

    float yaw = 0.0f;
    fm_vec3_t cam_t = g_player;
    uint32_t frames = 0;
    float fps = 60.0f;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);
        const float dt = 1.0f / 60.0f;

        kiln_clip_set_world(g_world, g_door_blocks ? WALL_BRUSHES + 1 : WALL_BRUSHES);
        const fm_vec3_t disp = {{ -in->stick_x * WALK_SPEED * dt, 0, in->stick_y * WALK_SPEED * dt }};
        g_player = kiln_clip_slide(g_player, disp, P_MINS, P_MAXS, 4);
        if (disp.v[0] * disp.v[0] + disp.v[2] * disp.v[2] > 1e-4f) yaw = fm_atan2f(disp.v[0], disp.v[2]);

        /* Events land before the actors update, per kiln_event's contract. */
        kiln_event_process(dt);
        kiln_actor_update_all(dt);
        for (int i = 0; i < 4; i++) if (g_pending[i].left > 0) g_pending[i].left -= dt;

        cam_t.v[0] += (g_player.v[0] * 0.6f - cam_t.v[0]) * 0.06f;
        cam_t.v[2] += (g_player.v[2] - cam_t.v[2]) * 0.06f;
        scene.cam_target = (fm_vec3_t){{ cam_t.v[0], 12, cam_t.v[2] + 30 }};
        scene.cam_pos = (fm_vec3_t){{ cam_t.v[0] * 0.8f, 120, cam_t.v[2] - 110 }};
        kiln_scene_update(&scene);

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        /* ── 3D ───────────────────────────────────────────────────── */
        kiln_frame_begin();
        kiln_scene_begin(&scene);
        kiln_transform_push(&xf_floor); kiln_prim_draw(&g_floor); kiln_transform_pop();
        kiln_transform_push(&xf_far);   kiln_prim_draw(&g_floor_far); kiln_transform_pop();
        for (int i = 0; i < WALL_BRUSHES; i++) kiln_prim_draw(&g_wall[i]);
        kiln_prim_draw(&g_chest);
        kiln_prim_draw(&g_chest_lid);
        kiln_actor_draw_all();
        xf_shadow.pos = (fm_vec3_t){{ g_player.v[0], 0.4f, g_player.v[2] }};
        kiln_transform_push(&xf_shadow); kiln_prim_draw(&g_shadow); kiln_transform_pop();
        xf_body.pos = g_player;
        xf_body.rot_angle = yaw;
        kiln_transform_push(&xf_body); kiln_prim_draw(&g_body); kiln_prim_draw(&g_nose); kiln_transform_pop();

        /* ── 2D ───────────────────────────────────────────────────── */
        kiln_gui_begin();

        /* The wire: while an OPEN is on its way, a line from switch to door. */
        int open_pending = 0;
        for (int i = 0; i < 4; i++)
            if (g_pending[i].left > 0 && g_pending[i].label[0] == 'D' && g_pending[i].label[5] == 'O') open_pending = 1;
        const float sdx = g_player.v[0] - SWITCH_AT.v[0], sdz = g_player.v[2] - SWITCH_AT.v[2];
        const int near_switch = sdx * sdx + sdz * sdz <= SWITCH_RANGE * SWITCH_RANGE;
        if (open_pending || near_switch) {
            kiln_dd_begin(&scene, SCREEN_W, SCREEN_H);
            if (open_pending)
                kiln_dd_line((fm_vec3_t){{ SWITCH_AT.v[0], 22, SWITCH_AT.v[2] }},
                             (fm_vec3_t){{ 0, 30, WALL_Z }}, RGBA32(0xFF, 0xE0, 0x40, 0xFF));
            if (near_switch && !open_pending)
                kiln_dd_text((fm_vec3_t){{ SWITCH_AT.v[0], 34, SWITCH_AT.v[2] }},
                             RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "A");
            kiln_dd_end();
        }

        const color_t ink = RGBA32(0xE8, 0xE8, 0xF0, 0xFF);
        const color_t teal = RGBA32(0x00, 0xF5, 0xD4, 0xFF);
        KilnActor *door = kiln_actor_resolve(door_h);
        const DoorState *ds = door ? (const DoorState *)door->state : NULL;
        kiln_gui_panel(8, 8, 160, 90, RGBA32(0x0C, 0x10, 0x1C, 0xFF), teal);
        kiln_gui_text(14, 21, teal, "KILN EVENT");
        kiln_gui_text(14, 34, ink, "door %s  queued %u",
                      !ds ? "-" : ds->open ? (ds->cur < -1.4f ? "open" : "opening")
                                          : (ds->cur < -0.1f ? "closing" : "shut"),
                      kiln_event_count());
        int row = 0;
        for (int i = 0; i < 4 && row < 3; i++) {
            const Pending *p = &g_pending[i];
            if (p->left <= 0) continue;
            const int y = 46 + row * 14;
            kiln_gui_text(14, y + 2, ink, "%-13s %4.1f", p->label, p->left);
            kiln_gui_rect(14, y + 5, 140, 3, RGBA32(0x30, 0x34, 0x44, 0xFF));
            kiln_gui_rect(14, y + 5, (int)(140 * (p->left / p->total)), 3, RGBA32(0xFF, 0xC0, 0x40, 0xFF));
            row++;
        }
        if (row == 0) kiln_gui_text(14, 48, RGBA32(0x90, 0x98, 0xB0, 0xFF), "no events pending");
        kiln_gui_text(14, 92, RGBA32(0x90, 0x98, 0xB0, 0xFF), "%4.1f fps", fps);

        if (kiln_input_scripted(1)) {
            kiln_gui_panel(SCREEN_W - 58, 8, 50, 16, RGBA32(0xC0, 0x30, 0x60, 0xFF),
                           RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
            kiln_gui_text(SCREEN_W - 49, 20, RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "DEMO");
        }
        kiln_gui_panel(8, SCREEN_H - 24, SCREEN_W - 16, 16,
                       RGBA32(0x0C, 0x10, 0x1C, 0xFF), RGBA32(0x8B, 0x5C, 0xF6, 0xFF));
        kiln_gui_text(14, SCREEN_H - 12, ink, "stick walk   A switch (stand at the pedestal)");

        kiln_gui_end();
        kiln_frame_end();
        kiln_audio_update();
    }
}
