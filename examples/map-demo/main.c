// SPDX-License-Identifier: MIT
//
// kiln_dict + kiln_map: a Quake-format arena, parsed on the console and walked.
//
// `assets/map_demo.map` (authored with ./dev map-emit) is loaded through the
// real kiln_map.c: brushes become the kiln_clip world, faces become polygons,
// and every non-worldspawn entity becomes a KilnRoomSpawn whose epairs sit in a
// KilnDict. The player spawns from `info_player_start`'s origin and angle.
//
// kiln_map faces carry no texture coordinates, so they arrive flat white.
// kiln_map_tint colours them once after load: floors by distance, raised tops
// warm, walls dark at the base and light at the top. It is the cheapest thing
// that makes brush geometry read as a place.
//
//   stick   walk (camera-relative)      A     jump — steps climb on their own
//   C < >   swing the camera            Z     brush + entity overlay
//   idle 2 s: the demo walks itself, up the steps and off the ledge
//
// Jump ROMs: .#map-demo-overlay boots with the overlay on and the tape
// running; .#map-demo-quake-test loads assets/quake_test.map — the parser's
// frozen fixture, whose player start sits INSIDE its only brush — and shows
// the spawn being lifted onto the brush instead of trapped in it.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_clip.h>
#include <kiln/kiln_dict.h>
#include <kiln/kiln_map.h>
#include <kiln/kiln_prim.h>
#include <kiln/kiln_debugdraw.h>

enum { JUMP_NONE, JUMP_OVERLAY, JUMP_QUAKE_TEST };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W 320
#define SCREEN_H 240

enum { PROFILE_START = 0, PROFILE_ENEMY = 1 };

/* Player collision box, relative to its centre. mins are NEGATIVE — passing
 * the half-extent for both, as this demo once did, shifts the box by a whole
 * half-extent on every axis. */
static const fm_vec3_t MINS = {{ -8, -14, -8 }};
static const fm_vec3_t MAXS = {{  8,  14,  8 }};
#define STEP_HEIGHT 14.0f
#define WALK_SPEED  110.0f
#define GRAVITY     700.0f
#define JUMP_SPEED  270.0f

// ── Attract tape ────────────────────────────────────────────────────────
// Forward over the plinth and up the steps onto the ledge, off its side,
// round a pillar while swinging the camera, and back. Raw stick counts.
static const KilnInputKey ATTRACT_KEYS[] = {
    { .frame =   0, .sy =  85 },
    { .frame = 250, .sx = -85 },                   // along the ledge, off the edge
    { .frame = 330, .sy = -85, .sx = -30 },
    { .frame = 430, .sy = -60, .cx = 70 },         // swing the camera round
    { .frame = 520, .sy =  85, .buttons = KILN_BTN_A },
    { .frame = 526, .sy =  85 },
    { .frame = 600, .sx =  85, .sy = 40 },
    { .frame = 720, .sy = -85, .cx = -70 },
    { .frame = 810, .sx = -60, .sy = -60 },
    { .frame = 900 },
};
static const KilnInputTape ATTRACT = { ATTRACT_KEYS, 10, 0 };

/* A spawn point placed inside a brush is unrecoverable for the player — kiln_clip
 * ignores a brush a trace STARTS inside, so they would walk out through its
 * faces. Lift it to the top of whatever brush contains it. */
static fm_vec3_t lift_out_of_brushes(const KilnMap *m, fm_vec3_t p)
{
    for (int pass = 0; pass < 4; pass++) {
        int moved = 0;
        for (int i = 0; i < m->brush_count; i++) {
            const KilnBrush *b = &m->brushes[i];
            if (p.v[0] + MAXS.v[0] > b->mins.v[0] && p.v[0] + MINS.v[0] < b->maxs.v[0] &&
                p.v[1] + MAXS.v[1] > b->mins.v[1] && p.v[1] + MINS.v[1] < b->maxs.v[1] &&
                p.v[2] + MAXS.v[2] > b->mins.v[2] && p.v[2] + MINS.v[2] < b->maxs.v[2]) {
                p.v[1] = b->maxs.v[1] - MINS.v[1] + 0.5f;
                moved = 1;
            }
        }
        if (!moved) break;
    }
    return p;
}

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);
    kiln_input_init();

    kiln_map_register_classname("info_player_start", PROFILE_START);
    kiln_map_register_classname("info_enemy", PROFILE_ENEMY);

    const char *path = (KILN_JUMP == JUMP_QUAKE_TEST) ? "rom:/maps/quake_test.map"
                                                      : "rom:/maps/map_demo.map";
    /* Zero-initialised: the failure path still reads it. */
    KilnMap map = { 0 };
    const int loaded = kiln_map_load(&map, path) == 0;
    if (!loaded) debugf("map-demo: kiln_map_load(%s) failed\n", path);
    else kiln_map_tint(&map, &(KilnMapTint){
        .floor = kiln_prim_rgba(0x68, 0x7E, 0x60), .floor_edge = kiln_prim_rgba(0x3C, 0x48, 0x38),
        .floor_y = 1.0f, .floor_radius = 362.0f,
        .top = kiln_prim_rgba(0xE0, 0xC0, 0x88),
        .wall_low = kiln_prim_rgba(0x4C, 0x42, 0x38), .wall_high = kiln_prim_rgba(0xA8, 0x94, 0x7C),
        .z_face_shade = 0.9f,
        .underside = kiln_prim_rgba(0x30, 0x2C, 0x28),
    });
    kiln_clip_set_world(map.brushes, map.brush_count);

    int start = -1;
    for (int i = 0; i < map.spawn_count; i++)
        if (map.spawns[i].profile_id == PROFILE_START) { start = i; break; }

    fm_vec3_t pos = start >= 0 ? map.spawns[start].pos : (fm_vec3_t){{ 0, 16, 0 }};
    const fm_vec3_t spawn_raw = pos;
    pos = lift_out_of_brushes(&map, pos);
    const int lifted = pos.v[1] != spawn_raw.v[1];
    float yaw = start >= 0 ? map.spawns[start].yaw : 0.0f;

    KilnScene scene;
    kiln_scene_init(&scene);
    kiln_prim_stage(&scene, RGBA32(0x2C, 0x30, 0x48, 0xFF), 380.0f, 900.0f);
    scene.fov_deg = 65.0f;
    scene.near_z = 8.0f;
    scene.far_z = 900.0f;

    KilnPrim body, nose, shadow;
    kiln_prim_box(&body, (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 8, 14, 8 }},
                  kiln_prim_rgba(0xFF, 0x70, 0x80), kiln_prim_rgba(0xE0, 0x40, 0x58),
                  kiln_prim_rgba(0x60, 0x10, 0x20));
    kiln_prim_box(&nose, (fm_vec3_t){{ 0, 6, 10 }}, (fm_vec3_t){{ 4, 3, 3 }},
                  kiln_prim_rgba(0xFF, 0xF0, 0xA0), kiln_prim_rgba(0xFF, 0xD0, 0x60),
                  kiln_prim_rgba(0x80, 0x60, 0x20));
    kiln_prim_floor(&shadow, 11.0f, 1, kiln_prim_rgba(0x10, 0x14, 0x14),
                    kiln_prim_rgba(0x10, 0x14, 0x14));
    KilnTransform body_xf, shadow_xf;
    kiln_transform_init(&body_xf);
    kiln_transform_init(&shadow_xf);

    int overlay = (KILN_JUMP == JUMP_OVERLAY);
    kiln_input_set_attract(1, &ATTRACT, KILN_JUMP == JUMP_OVERLAY ? 0 : 120);

    /* The camera heading starts behind the spawn's facing. Quake's `angle` is
     * degrees about +Y from +X; our heading is radians from +Z. */
    float cam_yaw = T3D_DEG_TO_RAD(90.0f) - yaw;
    yaw = cam_yaw; /* the body's heading uses the same convention as the camera */
    float vel_y = 0.0f;
    int on_ground = 0;
    fm_vec3_t cam_eye = pos;

    uint32_t frames = 0;
    float fps = 60.0f;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);
        const float dt = 1.0f / 60.0f;
        if (in->edges & KILN_BTN_Z) overlay = !overlay;
        cam_yaw += in->cstick_x * 2.2f * dt;
        if (in->buttons & KILN_BTN_CL) cam_yaw -= 2.0f * dt;
        if (in->buttons & KILN_BTN_CR) cam_yaw += 2.0f * dt;

        /* Camera-relative basis. The camera looks along f; screen right is
         * (-f.z, 0, f.x) for a Y-up view. */
        const fm_vec3_t f = {{ fm_sinf(cam_yaw), 0, fm_cosf(cam_yaw) }};
        const fm_vec3_t r = {{ -f.v[2], 0, f.v[0] }};
        const fm_vec3_t move = {{ (f.v[0] * in->stick_y + r.v[0] * in->stick_x) * WALK_SPEED * dt, 0,
                                  (f.v[2] * in->stick_y + r.v[2] * in->stick_x) * WALK_SPEED * dt }};
        const float move2 = move.v[0] * move.v[0] + move.v[2] * move.v[2];
        if (move2 > 1e-4f) yaw = fm_atan2f(move.v[0], move.v[2]);

        /* ── Walk, with step-up ───────────────────────────────────────────
         * Try the move flat and from STEP_HEIGHT up; keep whichever travelled
         * further, and settle the raised one back onto what it climbed. A step
         * is just a brush a little shorter than STEP_HEIGHT — kiln_map knows
         * nothing about stairs and does not need to. */
        if (move2 > 1e-4f) {
            const fm_vec3_t flat = kiln_clip_slide(pos, move, MINS, MAXS, 4);
            fm_vec3_t up = kiln_clip_slide(pos, (fm_vec3_t){{ 0, STEP_HEIGHT, 0 }}, MINS, MAXS, 1);
            up = kiln_clip_slide(up, move, MINS, MAXS, 4);
            const float dflat = (flat.v[0] - pos.v[0]) * (flat.v[0] - pos.v[0]) +
                                (flat.v[2] - pos.v[2]) * (flat.v[2] - pos.v[2]);
            const float dup = (up.v[0] - pos.v[0]) * (up.v[0] - pos.v[0]) +
                              (up.v[2] - pos.v[2]) * (up.v[2] - pos.v[2]);
            if (on_ground && dup > dflat + 0.01f) {
                const float rise = up.v[1] - pos.v[1];
                pos = kiln_clip_slide(up, (fm_vec3_t){{ 0, -rise, 0 }}, MINS, MAXS, 1);
            } else {
                pos = flat;
            }
        }

        /* ── Fall and jump ──────────────────────────────────────────────── */
        if (on_ground && (in->edges & KILN_BTN_A)) vel_y = JUMP_SPEED;
        vel_y -= GRAVITY * dt;
        if (vel_y < -600.0f) vel_y = -600.0f;
        const fm_vec3_t before = pos;
        pos = kiln_clip_slide(pos, (fm_vec3_t){{ 0, vel_y * dt, 0 }}, MINS, MAXS, 1);
        if (vel_y > 0 && pos.v[1] < before.v[1] + vel_y * dt - 0.01f) vel_y = 0; /* ceiling */
        const KilnTrace g = kiln_clip_ground(pos, MINS, MAXS);
        on_ground = g.fraction < 1.0f && vel_y <= 0.0f;
        if (on_ground) vel_y = 0.0f;
        if (pos.v[1] < -400.0f) { pos = lift_out_of_brushes(&map, spawn_raw); vel_y = 0; }

        /* ── Camera boom, pulled in by the world ───────────────────────────
         * kiln_clip_ray from the look point to where the eye wants to be; on a
         * hit the eye stops short of the wall instead of seeing through it. */
        const fm_vec3_t look = {{ pos.v[0], pos.v[1] + 14, pos.v[2] }};
        fm_vec3_t want = {{ look.v[0] - f.v[0] * 150, look.v[1] + 70, look.v[2] - f.v[2] * 150 }};
        const KilnTrace boom = kiln_clip_ray(look, want);
        if (boom.fraction < 1.0f) {
            const float k = boom.fraction * 0.9f;
            want = (fm_vec3_t){{ look.v[0] + (want.v[0] - look.v[0]) * k,
                                 look.v[1] + (want.v[1] - look.v[1]) * k,
                                 look.v[2] + (want.v[2] - look.v[2]) * k }};
        }
        for (int k = 0; k < 3; k++) cam_eye.v[k] += (want.v[k] - cam_eye.v[k]) * 0.18f;
        scene.cam_pos = cam_eye;
        scene.cam_target = look;
        kiln_scene_update(&scene);

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        /* ── 3D pass ───────────────────────────────────────────────── */
        kiln_frame_begin();
        kiln_scene_begin(&scene);

        kiln_map_draw(&map);

        const KilnTrace drop = kiln_clip_box(pos, (fm_vec3_t){{ pos.v[0], pos.v[1] - 400, pos.v[2] }},
                                             (fm_vec3_t){{ -1, -14, -1 }}, (fm_vec3_t){{ 1, 14, 1 }});
        shadow_xf.pos = (fm_vec3_t){{ pos.v[0], drop.endpos.v[1] - 13.5f, pos.v[2] }};
        kiln_transform_push(&shadow_xf);
        kiln_prim_draw(&shadow);
        kiln_transform_pop();

        body_xf.pos = pos;
        body_xf.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
        body_xf.rot_angle = yaw;
        kiln_transform_push(&body_xf);
        kiln_prim_draw(&body);
        kiln_prim_draw(&nose);
        kiln_transform_pop();

        /* ── 2D pass ───────────────────────────────────────────────── */
        kiln_gui_begin();

        if (overlay) {
            kiln_dd_begin(&scene, SCREEN_W, SCREEN_H);
            for (int i = 1; i < map.brush_count; i++)   /* skip the floor slab */
                kiln_dd_aabb(map.brushes[i].mins, map.brushes[i].maxs, RGBA32(0x60, 0xF0, 0xD0, 0xFF));
            for (int i = 0; i < map.spawn_count; i++) {
                const KilnRoomSpawn *s = &map.spawns[i];
                kiln_dd_axes(s->pos, 16.0f);
                kiln_dd_text(s->pos, RGBA32(0xFF, 0xFF, 0x80, 0xFF), "%s",
                             s->profile_id == PROFILE_START ? "start" : "enemy");
            }
            kiln_dd_end();
        }

        const color_t ink = RGBA32(0xE8, 0xE8, 0xF0, 0xFF);
        const color_t teal = RGBA32(0x00, 0xF5, 0xD4, 0xFF);
        kiln_gui_panel(8, 8, 172, 78, RGBA32(0x0C, 0x10, 0x1C, 0xFF), teal);
        kiln_gui_text(14, 21, teal, "KILN MAP");
        kiln_gui_text(66, 21, RGBA32(0x90, 0x98, 0xB0, 0xFF), "%s",
                      KILN_JUMP == JUMP_QUAKE_TEST ? "quake_test" : "map_demo");
        if (loaded) {
            kiln_gui_text(14, 34, ink, "brush %d face %d ent %d", map.brush_count,
                          map.face_count, map.spawn_count);
        } else {
            kiln_gui_text(14, 34, RGBA32(0xFF, 0x50, 0x50, 0xFF), "MAP DID NOT LOAD");
        }
        kiln_gui_text(14, 46, ink, "pos %4.0f %4.0f %4.0f", pos.v[0], pos.v[1], pos.v[2]);
        /* "angle" "90" is auto-typed to an int by kiln_dict, so read it as one
         * — a string read of it returns the default. */
        kiln_gui_text(14, 58, ink, "angle %d  %s",
                      start >= 0 ? (int)kiln_dict_get_int(&map.spawns[start].dict, "angle", 0) : 0,
                      lifted ? "spawn lifted" : (on_ground ? "ground" : "air"));
        kiln_gui_text(14, 70, RGBA32(0x90, 0x98, 0xB0, 0xFF), "%4.1f fps", fps);

        if (kiln_input_scripted(1)) {
            kiln_gui_panel(SCREEN_W - 58, 8, 50, 16, RGBA32(0xC0, 0x30, 0x60, 0xFF),
                           RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
            kiln_gui_text(SCREEN_W - 49, 20, RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "DEMO");
        }
        kiln_gui_panel(8, SCREEN_H - 24, SCREEN_W - 16, 16,
                       RGBA32(0x0C, 0x10, 0x1C, 0xFF), RGBA32(0x8B, 0x5C, 0xF6, 0xFF));
        kiln_gui_text(14, SCREEN_H - 12, ink, "stick walk  A jump  C cam  Z overlay");

        kiln_gui_end();
        kiln_frame_end();
    }
}
