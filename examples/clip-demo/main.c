// SPDX-License-Identifier: MIT
//
// fig_input + fig_clip: a box you push around a small stone-and-copper maze.
// It slides along walls instead of stopping dead — that is what
// fig_clip_slide buys over a single fig_clip_box trace — and the overlay
// shows why: the last trace as a line, its contact normal as an arrow, the
// brush it touched outlined, and a trail of where the box has been.
//
//   fig_input  -> one poll per frame, deadzoned stick + edge/level buttons,
//                  and an attract tape that drives the box after 2 s idle
//   fig_clip   -> swept AABB vs flat brush array, slab method, SlideMove
//   fig_surface+sound -> the contact sound changes on copper
//   fig_prim   -> the brushes, floor, player and blob shadow
//
// Jump ROM: .#clip-demo-corner boots wedged into the copper corner post with
// the stick held into it, so the contact normal is on screen with no pad.

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_clip.h>
#include <kiln/kiln_audio.h>
#include <kiln/kiln_surface.h>
#include <kiln/kiln_sound.h>
#include <kiln/kiln_prim.h>
#include <kiln/kiln_debugdraw.h>

enum { JUMP_NONE, JUMP_CORNER };
#ifndef KILN_JUMP
#define KILN_JUMP JUMP_NONE
#endif

#define SCREEN_W 320
#define SCREEN_H 240

enum { SURF_STONE = 0, SURF_COPPER = 1 };

// ── World brushes ───────────────────────────────────────────────────────
// A 200x200 room: four perimeter walls, two stone baffles that make a
// corridor, and three copper posts. Everything the player can touch is here
// and nowhere else — the renderer draws exactly these boxes.
static FigBrush g_brushes[] = {
    /* north wall */ { .mins = {{ -100, 0, -100 }}, .maxs = {{  100, 40,  -92 }}, .surface = SURF_STONE },
    /* south wall */ { .mins = {{ -100, 0,   92 }}, .maxs = {{  100, 40,  100 }}, .surface = SURF_STONE },
    /* west wall  */ { .mins = {{ -100, 0,  -92 }}, .maxs = {{  -92, 40,   92 }}, .surface = SURF_STONE },
    /* east wall  */ { .mins = {{   92, 0,  -92 }}, .maxs = {{  100, 40,   92 }}, .surface = SURF_STONE },
    /* baffle W   */ { .mins = {{  -92, 0,  -30 }}, .maxs = {{  -20, 24,  -22 }}, .surface = SURF_STONE },
    /* baffle E   */ { .mins = {{   20, 0,   22 }}, .maxs = {{   92, 24,   30 }}, .surface = SURF_STONE },
    /* post       */ { .mins = {{  -12, 0,   48 }}, .maxs = {{   12, 32,   60 }}, .surface = SURF_COPPER },
    /* post       */ { .mins = {{   40, 0,  -64 }}, .maxs = {{   56, 32,  -48 }}, .surface = SURF_COPPER },
    /* corner post*/ { .mins = {{   68, 0,   68 }}, .maxs = {{   92, 36,   92 }}, .surface = SURF_COPPER },
};
#define BRUSH_COUNT ((int)(sizeof(g_brushes) / sizeof(g_brushes[0])))

// ── Attract tape ────────────────────────────────────────────────────────
// Up the west corridor, along the baffle, round the post and back. Stick
// counts are raw (±85 full tilt); the camera faces +Z, so stick up is +Z.
static const FigInputKey ATTRACT_KEYS[] = {
    { .frame =   0, .sy =  85 },              // into the baffle: stops, slides
    { .frame =  70, .sx =  70, .sy =  60 },   // diagonal along its face
    { .frame = 170, .sy =  85 },              // up past it
    { .frame = 230, .sx = -80, .sy =  30 },   // into the west wall, slide north
    { .frame = 300, .sx =  85 },              // east along the south side
    { .frame = 420, .sx =  60, .sy =  60 },   // wedge into the copper corner
    { .frame = 480, .sy = -85 },              // back down the east corridor
    { .frame = 600, .sx = -85, .sy = -40 },   // into the east baffle
    { .frame = 680, .sx = -70, .sy = -70 },
    { .frame = 780 },                         // end: loop
};
static const FigInputTape ATTRACT = { ATTRACT_KEYS, 10, 0 };

// Held into the corner post forever: the jump ROM's latched state. Stick
// right is -X here (the camera looks down +Z), so +X+Z is up-LEFT.
static const FigInputKey CORNER_KEYS[] = { { .frame = 0, .sx = -60, .sy = 60 } };
static const FigInputTape CORNER = { CORNER_KEYS, 1, FIG_INPUT_NO_LOOP };

#define TRAIL_N 32

static fm_vec3_t brush_centre(const FigBrush *b)
{
    return (fm_vec3_t){{ (b->mins.v[0] + b->maxs.v[0]) * 0.5f,
                         (b->mins.v[1] + b->maxs.v[1]) * 0.5f,
                         (b->mins.v[2] + b->maxs.v[2]) * 0.5f }};
}

static fm_vec3_t brush_half(const FigBrush *b)
{
    return (fm_vec3_t){{ (b->maxs.v[0] - b->mins.v[0]) * 0.5f,
                         (b->maxs.v[1] - b->mins.v[1]) * 0.5f,
                         (b->maxs.v[2] - b->mins.v[2]) * 0.5f }};
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

int main(void)
{
    fig_engine_init(RESOLUTION_320x240);
    joypad_init();
    /* Mount the ROM's DragonFS before anything opens rom:/. The host resolves
     * rom:/ paths without it, so host renders never noticed; on console the
     * first fig_sfx_load asserted "File not found". */
    dfs_init(DFS_DEFAULT_LOCATION);
    fig_input_init();
    fig_audio_init(FIG_AUDIO_DEFAULT);

    /* Surface props + sound shaders: copper rings shorter and brighter. */
    int sfx_stone = fig_sfx_load("rom:/sfx/blip.wav64");
    int sfx_metal = fig_sfx_load("rom:/sfx/step.wav64");
    fig_surface_register(SURF_STONE,  &(FigSurfaceDef){ .friction = 0.9f, .footstep_sfx = sfx_stone });
    fig_surface_register(SURF_COPPER, &(FigSurfaceDef){ .friction = 0.6f, .footstep_sfx = sfx_metal });
    FigSoundShader shaders[] = {
        { .name = "hit_stone",  .wav64_path = "rom:/sfx/blip.wav64", .base_vol = 0.5f, .falloff_radius = 0.0f },
        { .name = "hit_copper", .wav64_path = "rom:/sfx/step.wav64", .base_vol = 0.8f, .falloff_radius = 0.0f },
    };
    fig_sound_init(shaders, 2);

    fig_clip_set_world(g_brushes, BRUSH_COUNT);

    /* ── Geometry: one prim per brush at its real size, so no transform
     * scales a normal. Stone tops are lighter than their sides so the maze
     * reads from above; copper glows warm against the cool fog. */
    static FigPrim brush_prim[sizeof(g_brushes) / sizeof(g_brushes[0])];
    for (int i = 0; i < BRUSH_COUNT; i++) {
        const int copper = g_brushes[i].surface == SURF_COPPER;
        fig_prim_box(&brush_prim[i], brush_centre(&g_brushes[i]), brush_half(&g_brushes[i]),
                      copper ? fig_prim_rgba(0xFF, 0xB8, 0x70) : fig_prim_rgba(0xC8, 0xD0, 0xE0),
                      copper ? fig_prim_rgba(0xC8, 0x70, 0x38) : fig_prim_rgba(0x78, 0x84, 0xA0),
                      fig_prim_rgba(0x20, 0x20, 0x28));
    }
    FigPrim floor_prim, player_prim, shadow_prim;
    fig_prim_floor(&floor_prim, 100.0f, 10,
                    fig_prim_rgba(0x5A, 0x6E, 0x78), fig_prim_rgba(0x4A, 0x5C, 0x68));
    fig_prim_box(&player_prim, (fm_vec3_t){{ 0, 0, 0 }}, (fm_vec3_t){{ 8, 8, 8 }},
                  fig_prim_rgba(0xFF, 0xE8, 0x40), fig_prim_rgba(0xE0, 0xA8, 0x10),
                  fig_prim_rgba(0x60, 0x40, 0x00));
    fig_prim_floor(&shadow_prim, 10.0f, 1,
                    fig_prim_rgba(0x0C, 0x12, 0x18), fig_prim_rgba(0x0C, 0x12, 0x18));

    FigScene scene;
    fig_scene_init(&scene);
    fig_prim_stage(&scene, RGBA32(0x1C, 0x26, 0x3C, 0xFF), 240.0f, 560.0f);
    scene.fov_deg = 60.0f;
    scene.near_z = 16.0f;
    scene.far_z = 560.0f;

    FigTransform player_xf, shadow_xf;
    fig_transform_init(&player_xf);
    fig_transform_init(&shadow_xf);

    /* The player's AABB, relative to its centre: mins are NEGATIVE. Passing
     * `half` for both (as this demo once did) offsets the collision box by a
     * full half-extent, so the box sinks into -X/-Z walls and stops short of
     * +X/+Z ones. */
    const fm_vec3_t mins = {{ -8, -8, -8 }};
    const fm_vec3_t maxs = {{  8,  8,  8 }};
    fm_vec3_t pos = {{ -56, 8, -64 }};
    const float speed = 90.0f; /* world units / second at full stick */

    if (KILN_JUMP == JUMP_CORNER) {
        pos = (fm_vec3_t){{ 56, 8, 56 }};
        fig_input_play(1, &CORNER);
    } else {
        fig_input_set_attract(1, &ATTRACT, 120);
    }

    /* A fixed camera heading (+Z) makes the stick basis constant. */
    const fm_vec3_t fwd = {{ 0, 0, 1 }};
    const fm_vec3_t right = {{ -1, 0, 0 }};

    FigTrace last_trace = { .fraction = 1.0f };
    fm_vec3_t last_disp = {{ 0, 0, 0 }};
    int touching = -1;

    fm_vec3_t trail[TRAIL_N];
    for (int i = 0; i < TRAIL_N; i++) trail[i] = pos;
    int trail_head = 0;
    int show_overlay = 1;

    fm_vec3_t cam_target = pos;
    float step_cd = 0.0f;
    uint32_t frames = 0;
    float fps = 60.0f;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        fig_input_update();
        const FigInput *in = fig_input_get(1);
        if (in->edges & FIG_BTN_Z) show_overlay = !show_overlay;

        const float dt = 1.0f / 60.0f;
        fm_vec3_t disp = {{
            (fwd.v[0] * in->stick_y + right.v[0] * in->stick_x) * speed * dt,
            0.0f,
            (fwd.v[2] * in->stick_y + right.v[2] * in->stick_x) * speed * dt,
        }};

        /* The first trace along the desired move is what a player perceives
         * as "the wall I'm hitting"; SlideMove then clips and retries. The
         * trace is cast a little further than one frame's move so a box
         * resting against a wall still reports the contact. */
        const float smag2 = in->stick_x * in->stick_x + in->stick_y * in->stick_y;
        if (smag2 > 0.01f) {
            /* Start one unit BACK from the box: a box already resting on a
             * wall begins its trace in contact, which fig_clip reports as
             * fraction 0 with no normal — the arrow would never appear
             * against the very wall it is pushing into. */
            last_disp = disp;
            const float len = fm_vec3_len(&disp);
            fm_vec3_t from = {{ pos.v[0] - disp.v[0] / len, pos.v[1], pos.v[2] - disp.v[2] / len }};
            fm_vec3_t probe = {{ pos.v[0] + disp.v[0] * 4, pos.v[1], pos.v[2] + disp.v[2] * 4 }};
            last_trace = fig_clip_box(from, probe, mins, maxs);
        }
        pos = fig_clip_slide(pos, disp, mins, maxs, 4);

        /* Which brush is the box against: its AABB grown by one unit. */
        touching = -1;
        for (int i = 0; i < BRUSH_COUNT; i++) {
            const FigBrush *b = &g_brushes[i];
            if (pos.v[0] + 9 > b->mins.v[0] && pos.v[0] - 9 < b->maxs.v[0] &&
                pos.v[2] + 9 > b->mins.v[2] && pos.v[2] - 9 < b->maxs.v[2]) {
                touching = i;
                break;
            }
        }

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }
        if (frames % 4 == 0) {
            trail[trail_head] = pos;
            trail_head = (trail_head + 1) % TRAIL_N;
        }

        /* Contact sound, throttled to ~3 Hz, picked by the touched surface. */
        step_cd -= dt;
        if (smag2 > 0.09f && last_trace.fraction < 0.999f && step_cd <= 0.0f &&
            last_trace.hitsurface <= SURF_COPPER) {
            fig_sound_play(last_trace.hitsurface == SURF_COPPER ? "hit_copper" : "hit_stone",
                            pos, 1.0f);
            step_cd = 0.35f;
        }

        /* Camera: high and behind, easing after the box, clamped inside the
         * room so a wall can never come between it and the player. */
        cam_target.v[0] += (pos.v[0] - cam_target.v[0]) * 0.08f;
        cam_target.v[2] += (pos.v[2] - cam_target.v[2]) * 0.08f;
        scene.cam_target = (fm_vec3_t){{ cam_target.v[0], 0, cam_target.v[2] + 16 }};
        scene.cam_pos = (fm_vec3_t){{ clampf(cam_target.v[0] * 0.5f, -80, 80), 150,
                                      clampf(cam_target.v[2] - 110, -88, 70) }};
        fig_scene_update(&scene);
        fig_sound_update_listener(scene.cam_pos, fwd);

        /* ── 3D pass ───────────────────────────────────────────────── */
        fig_frame_begin();
        fig_scene_begin(&scene);

        fig_prim_draw(&floor_prim);
        for (int i = 0; i < BRUSH_COUNT; i++) fig_prim_draw(&brush_prim[i]);

        shadow_xf.pos = (fm_vec3_t){{ pos.v[0], 0.5f, pos.v[2] }};
        fig_transform_push(&shadow_xf);
        fig_prim_draw(&shadow_prim);
        fig_transform_pop();

        player_xf.pos = pos;
        player_xf.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
        player_xf.rot_angle = 0.0f;
        fig_transform_push(&player_xf);
        fig_prim_draw(&player_prim);
        fig_transform_pop();

        /* ── 2D pass ───────────────────────────────────────────────── */
        fig_gui_begin();

        if (show_overlay) {
            fig_dd_begin(&scene, SCREEN_W, SCREEN_H);
            fm_vec3_t pts[TRAIL_N];
            for (int i = 0; i < TRAIL_N; i++) {
                pts[i] = trail[(trail_head + i) % TRAIL_N];
                pts[i].v[1] = 1.0f;
            }
            fig_dd_path(pts, TRAIL_N, RGBA32(0x60, 0xC0, 0xFF, 0xFF));

            fm_vec3_t ahead = {{ pos.v[0] + last_disp.v[0] * 20, pos.v[1],
                                 pos.v[2] + last_disp.v[2] * 20 }};
            fig_dd_line(pos, ahead, RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
            if (last_trace.fraction < 0.999f) {
                const fm_vec3_t e = last_trace.endpos;
                fm_vec3_t tip = {{ e.v[0] + last_trace.normal.v[0] * 24, e.v[1] + 12,
                                   e.v[2] + last_trace.normal.v[2] * 24 }};
                fm_vec3_t base = {{ e.v[0], e.v[1] + 12, e.v[2] }};
                fig_dd_line(base, tip, RGBA32(0xFF, 0x40, 0x80, 0xFF));
                fig_dd_point(tip, 3, RGBA32(0xFF, 0x40, 0x80, 0xFF));
            }
            if (touching >= 0) {
                fig_dd_aabb(g_brushes[touching].mins, g_brushes[touching].maxs,
                             g_brushes[touching].surface == SURF_COPPER
                                 ? RGBA32(0xFF, 0xB0, 0x40, 0xFF)
                                 : RGBA32(0x80, 0xFF, 0xE0, 0xFF));
            }
            fig_dd_end();
        }

        const color_t ink = RGBA32(0xE8, 0xE8, 0xF0, 0xFF);
        const color_t teal = RGBA32(0x00, 0xF5, 0xD4, 0xFF);
        fig_gui_panel(8, 8, 150, 66, RGBA32(0x0C, 0x10, 0x1C, 0xFF), teal);
        fig_gui_text(14, 21, teal, "KILN CLIP");
        fig_gui_text(14, 34, ink, "pos  %5.0f %5.0f", pos.v[0], pos.v[2]);
        fig_gui_text(14, 46, ink, "frac %4.2f %s", last_trace.fraction,
                      last_trace.fraction < 0.999f
                          ? (last_trace.hitsurface == SURF_COPPER ? "copper" : "stone")
                          : "clear");
        fig_gui_text(14, 58, ink, "n %4.1f %4.1f %4.1f", last_trace.normal.v[0],
                      last_trace.normal.v[1], last_trace.normal.v[2]);
        fig_gui_text(14, 70, RGBA32(0x90, 0x98, 0xB0, 0xFF), "%4.1f fps", fps);

        if (fig_input_scripted(1)) {
            fig_gui_panel(SCREEN_W - 58, 8, 50, 16, RGBA32(0xC0, 0x30, 0x60, 0xFF),
                           RGBA32(0xFF, 0xFF, 0xFF, 0xFF));
            fig_gui_text(SCREEN_W - 49, 20, RGBA32(0xFF, 0xFF, 0xFF, 0xFF), "DEMO");
        }

        fig_gui_panel(8, SCREEN_H - 24, SCREEN_W - 16, 16,
                       RGBA32(0x0C, 0x10, 0x1C, 0xFF), RGBA32(0x8B, 0x5C, 0xF6, 0xFF));
        fig_gui_text(14, SCREEN_H - 12, ink, "stick move   Z overlay   idle 2s: demo");

        fig_gui_end();
        fig_frame_end();

        fig_sound_update();
        fig_audio_update();
    }
}
