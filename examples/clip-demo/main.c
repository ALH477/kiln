// SPDX-License-Identifier: MIT
//
// Phase C step 1: kiln_input (deadzoned joypad wrapper with button edges) +
// kiln_clip (swept-AABB-vs-AABB-brushes collision with iterative SlideMove).
// One player box you push around a 5-brush room (4 perimeter walls + 1
// interior pillar). The box slides along the walls instead of stopping dead
// — that is what kiln_clip_slide buys you over a single kiln_clip_box trace.
// The HUD reports the last trace's fraction / hit-normal / surface so the
// collision state is readable, not just visible.
//
//   kiln_input  -> one poll per frame, deadzoned stick + edge/level buttons
//   kiln_clip   -> swept AABB vs flat brush array, slab method, SlideMove
//   kiln_surface+sound -> footstep SFX changes when stepping on metal pillar

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_clip.h>
#include <kiln/kiln_audio.h>
#include <kiln/kiln_surface.h>
#include <kiln/kiln_sound.h>

#include <malloc.h>

#define SCREEN_W 320
#define SCREEN_H 240

// ── World brushes ───────────────────────────────────────────────────────
// A 200×200 room with 4 perimeter walls + 1 interior pillar. Surface ids
// are placeholders (Phase 3 wires them to kiln_surface footstep SFX); 0 is the
// default "stone" surface the demo registers at index 0, 1 is "metal" for the
// pillar so Phase 3 can give it a different footstep without touching this
// file again.
static KilnBrush g_brushes[] = {
    /* north wall */ { .mins = {{ -100,  0, -100 }}, .maxs = {{  100, 40,  -96 }}, .surface = 0 },
    /* south wall */ { .mins = {{ -100,  0,   96 }}, .maxs = {{  100, 40,  100 }}, .surface = 0 },
    /* west wall  */ { .mins = {{ -100,  0, -100 }}, .maxs = {{  -96, 40,  100 }}, .surface = 0 },
    /* east wall  */ { .mins = {{   96, 0, -100 }}, .maxs = {{  100, 40,  100 }}, .surface = 0 },
    /* pillar     */ { .mins = {{  -30, 0,   20 }}, .maxs = {{  -10, 40,   40 }}, .surface = 1 },
};
#define BRUSH_COUNT (sizeof(g_brushes) / sizeof(g_brushes[0]))

// ── Rendering ───────────────────────────────────────────────────────────
// A unit cube (half-extent 1) at the origin. Each brush is drawn by pushing
// a transform that places its center and scales by its half-extents; the
// player is the same cube scaled to its half-extents. Two vertex buffers so
// walls and player can be different colours without rebuilding verts.
static const uint8_t CUBE_TRIS[12][3] = {
    {0,1,2},{2,3,0}, {4,6,5},{6,4,7},
    {0,4,5},{5,1,0}, {1,5,6},{6,2,1},
    {2,6,7},{7,3,2}, {3,7,4},{4,0,3},
};

static T3DVertPacked *make_unit_cube(uint32_t rgba)
{
    T3DVertPacked *v = malloc_uncached(sizeof(T3DVertPacked) * 4);
    const int16_t s = 1;
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

static void draw_box(const T3DVertPacked *verts, fm_vec3_t center, fm_vec3_t half)
{
    KilnTransform t;
    kiln_transform_init(&t);
    t.pos = center;
    t.scale = half; /* unit cube has half-extent 1, so scale == half-extent */
    kiln_transform_push(&t);
    t3d_vert_load(verts, 0, 8);
    for (int i = 0; i < 12; i++)
        t3d_tri_draw(CUBE_TRIS[i][0], CUBE_TRIS[i][1], CUBE_TRIS[i][2]);
    t3d_tri_sync();
    kiln_transform_pop();
    kiln_transform_free(&t);
}

// A floor quad so the room reads as a space, not a void. Two triangles in
// screen-space because the floor is at y=0 and the player box is at y=8, so
// the floor's verts are trivial.
static T3DVertPacked *make_floor(void)
{
    T3DVertPacked *v = malloc_uncached(sizeof(T3DVertPacked) * 2);
    const int16_t s = 100;
    fm_vec3_t n = {{ 0, 1, 0 }};
    uint8_t np = t3d_vert_pack_normal(&n);
    v[0] = (T3DVertPacked){
        .posA = { -s, 0, -s }, .rgbaA = 0x1A1A2EFF,
        .normA = np,
        .posB = {  s, 0, -s }, .rgbaB = 0x1A1A2EFF,
        .normB = np,
    };
    v[1] = (T3DVertPacked){
        .posA = { -s, 0,  s }, .rgbaA = 0x1A1A2EFF,
        .normA = np,
        .posB = {  s, 0,  s }, .rgbaB = 0x1A1A2EFF,
        .normB = np,
    };
    return v;
}

static void draw_floor(const T3DVertPacked *v)
{
    KilnTransform t;
    kiln_transform_init(&t);
    kiln_transform_push(&t);
    t3d_vert_load(v, 0, 4);
    /* (0,1,2) and (2,3,0) → CCW floor seen from above. */
    t3d_tri_draw(0, 1, 3);
    t3d_tri_draw(3, 2, 0);
    t3d_tri_sync();
    kiln_transform_pop();
    kiln_transform_free(&t);
}

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    kiln_input_init();
    kiln_audio_init(KILN_AUDIO_DEFAULT);

    /* Surface props + sound shaders. Pillar (surface 1) gets a sharper,
     * shorter footstep than the stone perimeter (surface 0). */
    int sfx_stone = kiln_sfx_load("rom:/sfx/blip.wav64");
    int sfx_metal = kiln_sfx_load("rom:/sfx/step.wav64");
    kiln_surface_register(0, &(KilnSurfaceDef){ .friction = 0.9f, .footstep_sfx = sfx_stone });
    kiln_surface_register(1, &(KilnSurfaceDef){ .friction = 0.6f, .footstep_sfx = sfx_metal });

    /* One shader per surface, so the same logical "footstep" reaches the
     * sound-shader path without duplicating sample paths. */
    KilnSoundShader shaders[] = {
        { .name = "step_stone", .wav64_path = "rom:/sfx/blip.wav64", .base_vol = 0.6f, .falloff_radius = 0.0f },
        { .name = "step_metal", .wav64_path = "rom:/sfx/step.wav64", .base_vol = 0.8f, .falloff_radius = 0.0f },
    };
    kiln_sound_init(shaders, 2);

    kiln_clip_set_world(g_brushes, BRUSH_COUNT);

    KilnScene scene;
    kiln_scene_init(&scene);
    /* Camera INSIDE the room, above wall height (walls are y=0..40), looking
     * down at the player from one corner. Outside-the-room cameras are
     * occluded by the perimeter walls — early screenshot was 99% clear colour
     * for exactly this reason. */
    scene.cam_pos    = (fm_vec3_t){{   60,  80,  -60 }};
    scene.cam_target = (fm_vec3_t){{    0,   8,    0 }};
    scene.far_z      = 400.0f;
    scene.ambient[3] = 255; /* make sure alpha is up */
    kiln_scene_update(&scene);

    T3DVertPacked *wall_v  = make_unit_cube(0x2E5B8CFF);
    T3DVertPacked *pill_v  = make_unit_cube(0x8C5B2EFF);
    T3DVertPacked *plyr_v  = make_unit_cube(0xF5D400FF);
    T3DVertPacked *floor_v = make_floor();

    /* Player state. The box is axis-aligned with half-extents (8,8,8) and
     * starts at the room's center, resting on the floor (y=8). */
    fm_vec3_t pos = (fm_vec3_t){{ 0, 8, 0 }};
    const fm_vec3_t half = (fm_vec3_t){{ 8, 8, 8 }};
    const float speed = 80.0f; /* world units / second at full stick */

    /* Camera-relative movement basis. cam→target projected on XZ and
     * normalised gives world-forward; right is its 90° rotate. Computed once
     * since the camera is fixed. */
    fm_vec3_t fwd = {{ scene.cam_target.v[0] - scene.cam_pos.v[0], 0,
                       scene.cam_target.v[2] - scene.cam_pos.v[2] }};
    fm_vec3_norm(&fwd, &fwd);
    fm_vec3_t right = {{ -fwd.v[2], 0, fwd.v[0] }};

    KilnTrace last_trace;
    last_trace.fraction = 1.0f;
    last_trace.normal = (fm_vec3_t){{ 0, 0, 0 }};
    last_trace.endpos = pos;
    last_trace.hitsurface = 0;

    /* Footstep state. A "footstep" here is really a wall-impact sound — the
     * player is a floating box with no floor, so we fire when pushing into
     * a brush, throttled so a sustained push doesn't machine-gun. The
     * surface id of the brush we hit picks the shader; the shader path
     * (kiln_sound_play) is what positions the sound, so both the surface
     * table and the shader table are exercised in one trigger. */
    float step_cd = 0.0f;
    uint8_t last_surface = 0xFF;
    const char *surf_name[2] = { "step_stone", "step_metal" };

    uint32_t frames = 0;
    float fps = 0.0f;
    uint32_t last_ticks = get_ticks();

    for (;;) {
        kiln_input_update();
        const KilnInput *in = kiln_input_get(1);

        const float dt = 1.0f / 60.0f;
        fm_vec3_t vel = {{
            (fwd.v[0]   * in->stick_y + right.v[0] * in->stick_x) * speed,
            0.0f,
            (fwd.v[2]   * in->stick_y + right.v[2] * in->stick_x) * speed,
        }};
        fm_vec3_t disp = {{ vel.v[0] * dt, 0, vel.v[2] * dt }};

        /* SlideMove: traces, clips velocity along contact normals, retries.
         * The last individual trace is kept for the HUD — it's the first
         * contact along the desired move, which is what a player perceives
         * as "the wall I'm hitting". */
        fm_vec3_t end;
        end.v[0] = pos.v[0] + disp.v[0];
        end.v[1] = pos.v[1] + disp.v[1];
        end.v[2] = pos.v[2] + disp.v[2];
        last_trace = kiln_clip_box(pos, end, half, half);
        pos = kiln_clip_slide(pos, disp, half, half, 4);

        if (++frames % 30 == 0) {
            uint32_t now = get_ticks();
            fps = 30.0f / ((float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND);
            last_ticks = now;
        }

        /* Footstep: fire on wall contact while pushing, throttled to ~3 Hz.
         * Squared stick magnitude avoids sqrt; 0.3² is the "pushing" gate. */
        const float smag2 = in->stick_x * in->stick_x + in->stick_y * in->stick_y;
        step_cd -= dt;
        if (smag2 > 0.09f && last_trace.fraction < 0.999f && step_cd <= 0.0f
            && last_trace.hitsurface < 2) {
            kiln_sound_play(surf_name[last_trace.hitsurface], pos, 1.0f);
            step_cd = 0.35f;
            last_surface = last_trace.hitsurface;
        }

        /* Listener = camera. Positional shaders need the ear and facing each
         * frame so vol/pan can be recomputed for still-playing channels. */
        kiln_sound_update_listener(scene.cam_pos, fwd);

        /* ── 3D pass ───────────────────────────────────────────────── */
        kiln_frame_begin();
        kiln_scene_begin(&scene);

        draw_floor(floor_v);

        /* Walls (perimeter) and the pillar, drawn from their AABBs. The
         * pillar gets its own buffer so a Phase 3 surface tint can swap
         * colour without touching the perimeter render. */
        for (int i = 0; i < 4; i++) {
            fm_vec3_t c, h;
            c.v[0] = (g_brushes[i].mins.v[0] + g_brushes[i].maxs.v[0]) * 0.5f;
            c.v[1] = (g_brushes[i].mins.v[1] + g_brushes[i].maxs.v[1]) * 0.5f;
            c.v[2] = (g_brushes[i].mins.v[2] + g_brushes[i].maxs.v[2]) * 0.5f;
            h.v[0] = (g_brushes[i].maxs.v[0] - g_brushes[i].mins.v[0]) * 0.5f;
            h.v[1] = (g_brushes[i].maxs.v[1] - g_brushes[i].mins.v[1]) * 0.5f;
            h.v[2] = (g_brushes[i].maxs.v[2] - g_brushes[i].mins.v[2]) * 0.5f;
            draw_box(wall_v, c, h);
        }
        {
            const KilnBrush *b = &g_brushes[4];
            fm_vec3_t c = {{ (b->mins.v[0] + b->maxs.v[0]) * 0.5f,
                             (b->mins.v[1] + b->maxs.v[1]) * 0.5f,
                             (b->mins.v[2] + b->maxs.v[2]) * 0.5f }};
            fm_vec3_t h = {{ (b->maxs.v[0] - b->mins.v[0]) * 0.5f,
                             (b->maxs.v[1] - b->mins.v[1]) * 0.5f,
                             (b->maxs.v[2] - b->mins.v[2]) * 0.5f }};
            draw_box(pill_v, c, h);
        }

        draw_box(plyr_v, pos, half);

        /* ── 2D pass ───────────────────────────────────────────────── */
        kiln_gui_begin();

        kiln_gui_panel(8, 8, 200, 78,
                      RGBA32(10, 10, 24, 200), RGBA32(0, 245, 212, 255));
        kiln_gui_text(14, 22, RGBA32(0, 245, 212, 255), "KILN CLIP + INPUT");
        kiln_gui_text(14, 34, RGBA32(232, 232, 240, 255), "fps  %5.1f", fps);
        kiln_gui_text(14, 46, RGBA32(232, 232, 240, 255),
                     "pos  %6.1f %6.1f", pos.v[0], pos.v[2]);
        kiln_gui_text(14, 58, RGBA32(232, 232, 240, 255),
                     "frac %4.2f  surf %d", last_trace.fraction, last_trace.hitsurface);
        kiln_gui_text(14, 70, RGBA32(232, 232, 240, 255),
                     "n    %4.2f %4.2f %4.2f",
                     last_trace.normal.v[0], last_trace.normal.v[1], last_trace.normal.v[2]);

        kiln_gui_panel(8, SCREEN_H - 28, SCREEN_W - 16, 20,
                      RGBA32(10, 10, 24, 200), RGBA32(139, 92, 246, 255));
        kiln_gui_text(14, SCREEN_H - 18, RGBA32(232, 232, 240, 255),
                     "stick: move   A: nop   B: nop   (Phase 5 wires Z-target)");

        kiln_gui_end();
        kiln_frame_end();

        kiln_sound_update();
        kiln_audio_update();
    }
}