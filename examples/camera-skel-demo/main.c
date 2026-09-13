// SPDX-License-Identifier: MIT
//
// A goblin walks a lit courtyard, with the camera on a spring arm behind him.
//
//   kiln_skel    -> goblin.t3dm's Idle and Walk clips, blended by speed; the
//                   walk also plays faster the harder the stick is pushed
//   kiln_camera  -> OoT's trailing boom (NORMAL mode). Its heading lags his
//                   facing, so a turn swings the camera round over several
//                   frames; its collision is ON, so a pillar or the wall pulls
//                   the eye in instead of letting it pass through
//   kiln_clip    -> the floor, walls and pillars are brushes; he slides round
//                   the pillars and along the walls
//   kiln_audio   -> step.wav64 on each footfall, panned a little per foot
//   kiln_prim    -> every piece of the courtyard is the brush it collides as
//   kiln_input   -> camera-relative stick, and an attract tape: a figure 8
//
// The stick is relative to the CAMERA, not the world: forward is where the
// camera looks. That is what makes a trailing camera work — with a world-space
// stick, the camera swinging round behind a turn would change what "up" means
// in the middle of the turn, and the two fight.
//
// Loose DFS, not StreamDB, for the goblin: an animated model is not single-
// file. Tiny3D's t3danim.c streams clip data from sidecar `.sdata` files via
// asset_fopen(path), a hardcoded DFS open — see CLAUDE.md's "Datafiles".
//
// Jump ROMs:
//   .#camera-skel-demo-walk  the figure 8, forced from boot
//   .#camera-skel-demo-rig   tools/gen_skel_gltf.py's hand-built 2-bone rig
//                            (idle/swing) in place of the goblin, walking a
//                            circle — the minimum skinned asset, kept booting

#include <libdragon.h>
#include <t3d/t3dmodel.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_camera.h>
#include <kiln/kiln_skel.h>
#include <kiln/kiln_clip.h>
#include <kiln/kiln_audio.h>
#include <kiln/kiln_prim.h>

enum { JUMP_NONE, JUMP_WALK, JUMP_RIG };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W 320
#define SCREEN_H 240

#define WALK_SPEED    30.0f   /* world units / second at full stick           */
#define TURN_SPEED    8.0f    /* facing damper, the same shape as the camera's */
#define STEP_LENGTH   13.0f   /* units between footfalls at this scale         */
#define YARD          230.0f  /* courtyard half-extent, inside the walls        */

/* goblin.py's rig stands ~2.2 Blender units on its feet, 140 model units at
 * baseScale 64; at 0.24 he is ~34 units tall. The origin is between his feet,
 * so the transform's y IS the floor height. */
#define GOBLIN_SCALE  0.24f
/* The 2-bone rig is built at baseScale 32 and is about as tall already. */
#define RIG_SCALE     0.55f

/* Authored facing: goblin.py puts his nose on Blender +Y, which the glTF
 * export turns into model -Z. KilnActor's yaw convention is 0 = +Z, so the
 * model is drawn half a turn round from the yaw it walks along. */
#define MODEL_YAW_OFFSET 3.14159265f

// ── The courtyard ───────────────────────────────────────────────────────
// Everything solid is here and nowhere else: the renderer draws these boxes
// and the clip world collides with the same array.
enum { B_FLOOR, B_WALL_N, B_WALL_S, B_WALL_W, B_WALL_E };
static KilnBrush g_brushes[] = {
    [B_FLOOR]  = { .mins = {{ -YARD - 16, -16, -YARD - 16 }}, .maxs = {{ YARD + 16,  0, YARD + 16 }} },
    [B_WALL_N] = { .mins = {{ -YARD - 16,   0,  YARD      }}, .maxs = {{ YARD + 16, 44, YARD + 16 }} },
    [B_WALL_S] = { .mins = {{ -YARD - 16,   0, -YARD - 16 }}, .maxs = {{ YARD + 16, 44, -YARD     }} },
    [B_WALL_W] = { .mins = {{  YARD,        0, -YARD      }}, .maxs = {{ YARD + 16, 44, YARD      }} },
    [B_WALL_E] = { .mins = {{ -YARD - 16,   0, -YARD      }}, .maxs = {{ -YARD,     44, YARD      }} },
    /* four tall pillars clear of the figure 8, two short plinths inside it */
    { .mins = {{ -122, 0,  110 }}, .maxs = {{ -98, 96,  134 }} },
    { .mins = {{   98, 0,  110 }}, .maxs = {{ 122, 96,  134 }} },
    { .mins = {{ -122, 0, -134 }}, .maxs = {{ -98, 96, -110 }} },
    { .mins = {{   98, 0, -134 }}, .maxs = {{ 122, 96, -110 }} },
    { .mins = {{ -180, 0,  -12 }}, .maxs = {{ -156, 22,  12 }} },
    { .mins = {{  156, 0,  -12 }}, .maxs = {{  180, 22,  12 }} },
};
#define BRUSH_COUNT ((int)(sizeof(g_brushes) / sizeof(g_brushes[0])))

// ── Tapes ───────────────────────────────────────────────────────────────
// A figure 8: stick forward and a little right for one loop, forward and a
// little left for the other. The stick is camera-relative and the camera
// trails his heading, so a steady diagonal is a steady circle. The loop
// length was solved offline against this file's own movement and kiln_camera
// NORMAL's dampers so one full 8 returns to where it began (within half a
// unit per cycle), spanning about 234 x 118 units around the spawn.
static const KilnInputKey EIGHT_KEYS[] = {
    { .frame =    0, .sx =  18, .sy = 83 },
    { .frame =  780, .sx = -18, .sy = 83 },
    { .frame = 1560 },
};
static const KilnInputTape EIGHT = { EIGHT_KEYS, 3, 0 };

// The RIG jump's circle: half tilt, so the swing blend sits mid-way.
static const KilnInputKey CIRCLE_KEYS[] = { { .frame = 0, .sx = 20, .sy = 42 } };
static const KilnInputTape CIRCLE = { CIRCLE_KEYS, 1, KILN_INPUT_NO_LOOP };

static fm_vec3_t brush_centre(const KilnBrush *b)
{
    return (fm_vec3_t){{ (b->mins.v[0] + b->maxs.v[0]) * 0.5f,
                         (b->mins.v[1] + b->maxs.v[1]) * 0.5f,
                         (b->mins.v[2] + b->maxs.v[2]) * 0.5f }};
}

static fm_vec3_t brush_half(const KilnBrush *b)
{
    return (fm_vec3_t){{ (b->maxs.v[0] - b->mins.v[0]) * 0.5f,
                         (b->maxs.v[1] - b->mins.v[1]) * 0.5f,
                         (b->maxs.v[2] - b->mins.v[2]) * 0.5f }};
}

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);
    asset_init_compression(2);   /* mkBlenderModel / mkModel: mkasset -c 2 */
    kiln_input_init();
    kiln_audio_init(KILN_AUDIO_DEFAULT);

    const int rig = KILN_JUMP == JUMP_RIG;
    int sfx_step = kiln_sfx_load("rom:/sfx/step.wav64");

    /* One model and one KilnSkel. The goblin's clips are Idle/Walk; the
     * hand-built rig's are idle/swing — same two-slot blend, same code. */
    T3DModel *model = t3d_model_load(rig ? "rom:/models/rig.t3dm" : "rom:/models/goblin.t3dm");
    KilnSkel skel;
    kiln_skel_create(&skel, model);
    kiln_skel_play(&skel, rig ? "idle" : "Idle", true);
    kiln_skel_play_blend(&skel, rig ? "swing" : "Walk", true);
    const float model_scale = rig ? RIG_SCALE : GOBLIN_SCALE;

    kiln_clip_set_world(g_brushes, BRUSH_COUNT);

    /* ── Geometry: one prim per brush at its real size, plus a checker
     * floor over the floor brush's top face. Sandstone walls, darker
     * pillars, warm plinths; mid-tone throughout, because the console is
     * much darker than any host preview of it. */
    static KilnPrim brush_prim[sizeof(g_brushes) / sizeof(g_brushes[0])];
    for (int i = 1; i < BRUSH_COUNT; i++) {
        const int wall = i <= B_WALL_E;
        const int plinth = g_brushes[i].maxs.v[1] < 30;
        kiln_prim_box(&brush_prim[i], brush_centre(&g_brushes[i]), brush_half(&g_brushes[i]),
                      wall ? kiln_prim_rgba(0xE0, 0xC8, 0x9C)
                           : plinth ? kiln_prim_rgba(0xF0, 0xA8, 0x60) : kiln_prim_rgba(0xD8, 0xD0, 0xC4),
                      wall ? kiln_prim_rgba(0xB0, 0x94, 0x6C)
                           : plinth ? kiln_prim_rgba(0xC0, 0x70, 0x38) : kiln_prim_rgba(0x98, 0x90, 0x8C),
                      kiln_prim_rgba(0x30, 0x28, 0x20));
    }
    KilnPrim floor_prim, shadow_prim;
    /* Light flagstones: 0x9C/0x84 read near-black-green in the first Ares
     * capture, where the walls at the same brightness looked fine. */
    kiln_prim_floor(&floor_prim, YARD, 12,
                    kiln_prim_rgba(0xE8, 0xDC, 0xB8), kiln_prim_rgba(0xC8, 0xBC, 0x9C));
    kiln_prim_floor(&shadow_prim, 9.0f, 1,
                    kiln_prim_rgba(0x30, 0x34, 0x28), kiln_prim_rgba(0x30, 0x34, 0x28));

    KilnScene scene;
    kiln_scene_init(&scene);
    kiln_prim_stage(&scene, RGBA32(0x5C, 0x70, 0x90, 0xFF), 260.0f, 620.0f);
    scene.fov_deg = 62.0f;
    scene.near_z = 8.0f;
    scene.far_z = 640.0f;

    /* The boom, in this world's units: kiln_camera_init's defaults (6 back,
     * 3 up) are sized for a one-unit character. */
    KilnCamera cam;
    kiln_camera_init(&cam);
    cam.distance = 96.0f;
    cam.height = 46.0f;
    cam.look_height = 22.0f;
    kiln_camera_set_collision(&cam, 1);

    /* His collision box, relative to his feet. mins are NEGATIVE; the bottom
     * sits one unit above the floor so a trace never starts inside it. */
    const fm_vec3_t mins = {{ -7, 1, -7 }};
    const fm_vec3_t maxs = {{  7, 32,  7 }};
    fm_vec3_t pos = {{ 0, 0, 8 }};
    float yaw = 0.0f;
    kiln_camera_snap(&cam, pos, yaw);

    if (KILN_JUMP == JUMP_WALK) kiln_input_play(1, &EIGHT);
    else if (rig) kiln_input_play(1, &CIRCLE);
    else kiln_input_set_attract(1, &EIGHT, 360);

    KilnTransform body_xf, shadow_xf;
    kiln_transform_init(&body_xf);
    kiln_transform_init(&shadow_xf);

    float speed_norm = 0.0f;   /* 0..1, drives the blend and the HUD         */
    float stride = 0.0f;       /* distance since the last footfall            */
    int foot = 0;
    uint32_t frames = 0;
    float fps = 60.0f;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);
        const float dt = 1.0f / 60.0f;

        /* Camera-relative basis, from where the camera actually is this
         * frame. Looking down +Z, screen-right is -X: right = (-fwd.z, fwd.x). */
        fm_vec3_t fwd = {{ cam.look.v[0] - cam.eye.v[0], 0, cam.look.v[2] - cam.eye.v[2] }};
        fm_vec3_norm(&fwd, &fwd);
        const fm_vec3_t right = {{ -fwd.v[2], 0, fwd.v[0] }};

        fm_vec3_t dir = {{ fwd.v[0] * in->stick_y + right.v[0] * in->stick_x, 0,
                           fwd.v[2] * in->stick_y + right.v[2] * in->stick_x }};
        float mag = fm_vec3_len(&dir);
        const float target_norm = mag > 1.0f ? 1.0f : mag;

        fm_vec3_t moved = pos;
        if (mag > 0.01f) {
            dir.v[0] /= mag;
            dir.v[2] /= mag;
            const float step = WALK_SPEED * target_norm * dt;
            fm_vec3_t disp = {{ dir.v[0] * step, 0, dir.v[2] * step }};
            moved = kiln_clip_slide(pos, disp, mins, maxs, 4);
            const float t = TURN_SPEED * dt;
            yaw = fm_lerp_angle(yaw, fm_atan2f(dir.v[0], dir.v[2]), t > 1.0f ? 1.0f : t);
        }
        /* Blend by distance actually covered, not by stick: walking into a
         * pillar is standing still, and should look like it. */
        fm_vec3_t delta = {{ moved.v[0] - pos.v[0], 0, moved.v[2] - pos.v[2] }};
        const float covered = fm_vec3_len(&delta);
        pos = moved;
        const float actual_norm = covered / (WALK_SPEED * dt);
        speed_norm += ((actual_norm > 1.0f ? 1.0f : actual_norm) - speed_norm) * 0.15f;

        stride += covered;
        if (stride >= STEP_LENGTH) {
            stride -= STEP_LENGTH;
            foot = !foot;
            if (sfx_step >= 0)
                kiln_sfx_play_ex(sfx_step, -1, 1, 0.45f + 0.35f * speed_norm, foot ? 0.42f : 0.58f);
        }

        kiln_skel_set_blend(&skel, speed_norm);
        kiln_skel_update(&skel, dt * (1.0f + 0.35f * speed_norm));

        kiln_camera_update(&cam, pos, yaw, dt);
        kiln_camera_apply(&cam, &scene);
        kiln_scene_update(&scene);

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        /* ── 3D pass ─────────────────────────────────────────────────── */
        kiln_frame_begin();
        kiln_scene_begin(&scene);

        kiln_prim_draw(&floor_prim);
        for (int i = 1; i < BRUSH_COUNT; i++) kiln_prim_draw(&brush_prim[i]);

        shadow_xf.pos = (fm_vec3_t){{ pos.v[0], 0.5f, pos.v[2] }};
        kiln_transform_push(&shadow_xf);
        kiln_prim_draw(&shadow_prim);
        kiln_transform_pop();

        body_xf.pos = pos;
        body_xf.scale = (fm_vec3_t){{ model_scale, model_scale, model_scale }};
        body_xf.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
        body_xf.rot_angle = yaw + MODEL_YAW_OFFSET;
        kiln_transform_push(&body_xf);
        kiln_skel_draw(&skel);
        kiln_transform_pop();

        /* ── 2D pass ─────────────────────────────────────────────────── */
        kiln_gui_begin();
        const color_t ink = RGBA32(0xE8, 0xE8, 0xF0, 0xFF);
        const color_t gold = RGBA32(0xFF, 0xC8, 0x60, 0xFF);
        const color_t dim = RGBA32(0x98, 0xA0, 0xB4, 0xFF);

        /* How far the collision has pulled the boom in, from its 105-unit
         * rest length (96 back, 46 up): the camera's side of the demo. */
        fm_vec3_t boom = {{ cam.eye.v[0] - cam.look.v[0], cam.eye.v[1] - cam.look.v[1],
                            cam.eye.v[2] - cam.look.v[2] }};
        const float boom_len = fm_vec3_len(&boom);

        kiln_gui_panel(8, 8, 142, 54, RGBA32(0x14, 0x18, 0x24, 0xFF), gold);
        kiln_gui_text(14, 21, gold, rig ? "KILN SKEL - RIG" : "KILN COURTYARD");
        kiln_gui_text(14, 33, ink, "walk %3d%%  %s", (int)(speed_norm * 100.0f + 0.5f),
                      speed_norm > 0.5f ? (rig ? "swing" : "Walk") : (rig ? "idle" : "Idle"));
        kiln_gui_text(14, 45, ink, "boom %3.0f%s", boom_len, boom_len < 90.0f ? " pulled" : "");
        kiln_gui_text(14, 57, dim, "%4.1f fps", fps);

        if (kiln_input_scripted(1)) {
            kiln_gui_panel(SCREEN_W - 58, 8, 50, 16, RGBA32(0xC0, 0x30, 0x60, 0xFF),
                           RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
            kiln_gui_text(SCREEN_W - 49, 20, RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "DEMO");
        }

        kiln_gui_panel(8, SCREEN_H - 24, SCREEN_W - 16, 16,
                       RGBA32(0x14, 0x18, 0x24, 0xFF), RGBA32(0x60, 0x90, 0xC0, 0xFF));
        kiln_gui_text(14, SCREEN_H - 12, ink, "stick walk (camera-relative)  idle 6s: demo");

        kiln_gui_end();
        kiln_frame_end();
        kiln_audio_update();
    }
}
