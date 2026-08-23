/* SPDX-License-Identifier: MIT
 *
 * kiln_splash.c — see kiln_splash.h.
 */

#include "kiln_splash.h"

#include <fmath.h>
#include <libdragon.h>
#include <string.h>
#include <t3d/t3d.h>
#include <t3d/t3dmodel.h>

#include "kiln_audio.h"
#include "kiln_gui.h"

/* ── The beat ──────────────────────────────────────────────────────────
 * One constant, referenced by the assembly, the flash and the jingle
 * trigger. The N64's boot works because picture and sound resolve on the
 * same frame; keeping the three uses of that moment as one number is what
 * stops them drifting apart when someone retimes the animation.
 *
 * The jingle's own chord lands 1.25 s into the render (dsp/kiln_jingle.dsp),
 * so it is started at t=0 and the picture is timed to meet it rather than
 * the other way round — audio cannot be nudged a frame at runtime, and
 * geometry can. */
#define BEAT        1.25f

#define T_LINE      1.60f   /* publisher line begins to fade up */
#define T_FADE_OUT  3.20f
#define T_END       3.80f

#define SPIN_TURNS  1.25f   /* revolutions before it settles */

/* A reserved channel, not an auto-allocated one.
 *
 * kiln_sfx_play(-1) picks the first idle channel in [0, sfx_channels), which
 * is the same pool a game's ambience bed may already be sitting in — and a
 * one-shot that lands on top of a looping bed leaves one of the two with a
 * sample buffer and no reader. Owning a channel outright means the splash's
 * audio cannot collide with anything the game is doing, and means the
 * splash can STOP it when it is finished, which is the other half of the
 * fix (see kiln_splash_update).
 *
 * Channel 2, not 1: a STEREO waveform occupies two adjacent mixer
 * channels, so a game holding channel 0 for a bed may really be holding
 * 0 and 1. Leaving a gap costs nothing and removes a collision that
 * presents as an assert deep inside mixer_poll rather than as anything
 * resembling "two sounds overlapped". */
#define SPLASH_CH 2

static T3DModel   *g_model;
static int         g_jingle = -1;
static int         g_jingle_stopped;
static const char *g_line;

static float g_t;
static int   g_done;
static int   g_started;
static int   g_flash;      /* frames of white left after the beat */

static KilnTransform g_xform;
static int          g_xform_ready;

/* The flame gets its own transform stacked on top of the body's, so it can
 * flicker independently of the settle animation rather than being baked
 * into the mesh. Looked up once, not every frame: t3d_model_get_object
 * walks the model's object chunks by name, which is model-parse work with
 * no reason to repeat 60 times a second for a pointer that cannot change
 * mid-splash. NULL is a valid outcome — a model built before the flame
 * object existed (or a caller's own custom model) draws as one rigid piece,
 * exactly as this module always has. */
static T3DObject     *g_kiln_obj;
static T3DObject     *g_flame_obj;
static T3DObject     *g_plate_obj;
static int             g_objects_looked_up;
static KilnTransform   g_flame_xform;
static int             g_flame_xform_ready;

void kiln_splash_init(T3DModel *model, int jingle_sfx, const char *line)
{
    g_model = model;
    g_jingle = jingle_sfx;
    g_line = line;
    g_t = 0.0f;
    g_done = 0;
    g_started = 0;
    g_flash = 0;
    g_objects_looked_up = 0;
    g_kiln_obj = NULL;
    g_flame_obj = NULL;
    g_plate_obj = NULL;
}

int kiln_splash_done(void) { return g_done; }

/* Ease used by the assembly: fast in, hard stop. The pieces arrive like
 * they were thrown, not like they were animated — that overshoot-free
 * deceleration is what the N64 logo's snap into place actually is. */
static float ease_out(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

/* Two incommensurate frequencies beating against each other, rather than one
 * visible pulse — the cheapest thing that does not look like a metronome.
 * Two fm_sinf calls a frame is not a hot-path cost: this runs for at most
 * 3.8 seconds once at boot. Centred on 1.0 so callers can multiply a scale
 * or a colour by it directly. */
static float flame_flicker(float t)
{
    return 1.0f + 0.10f * fm_sinf(t * 26.0f) + 0.06f * fm_sinf(t * 41.0f + 1.7f);
}

void kiln_splash_update(float dt, const KilnInput *in)
{
    if (g_done) return;

    if (!g_started) {
        g_started = 1;
        g_jingle_stopped = 0;
        /* Highest priority: this is the only sound playing, and a boot
         * jingle losing its channel to anything is not a failure mode
         * worth allowing. */
        if (g_jingle >= 0) kiln_sfx_play(g_jingle, SPLASH_CH, 255);
    }

    const float prev = g_t;
    g_t += dt;

    /* The flash, on the frame the beat is crossed. */
    if (prev < BEAT && g_t >= BEAT) g_flash = 1;

    /* Release the channel the moment the splash is over, whether it ran to
     * the end or was skipped.
     *
     * Leaving a FINISHED one-shot in the mixer is what killed the ROM a
     * couple of seconds after boot: mixer_poll kept asking that channel to
     * extend its sample buffer and libdragon asserted
     * "samplebuffer_get: no reader to extend". The looping ambience bed
     * never showed it because a loop never ends. So the rule is: whoever
     * starts a one-shot on a reserved channel stops it too. */
    const int finishing = (in && in->edges) || g_t >= T_END;
    if (finishing && !g_jingle_stopped) {
        g_jingle_stopped = 1;
        if (g_jingle >= 0) kiln_sfx_stop(SPLASH_CH);
    }
    if (in && in->edges) { g_done = 1; return; }   /* skipped */
    if (g_t >= T_END) g_done = 1;
}

void kiln_splash_apply(KilnScene *scene)
{
    /* A fixed three-quarter view. The logo moves, the camera does not —
     * the original boot holds its frame and lets the mark do the work,
     * and a camera that also moves would just make the shot busy. */
    /* The logo model is built at baseScale 64 (model units per Blender
     * unit), so it is ~200 units wide and everything here is in those same
     * units. Framing numbers and model scale have to move together — that
     * is exactly what went wrong when the model was built at baseScale 1. */
    scene->cam_pos = (fm_vec3_t){ { 0.0f, 22.0f, 410.0f } };
    scene->cam_target = (fm_vec3_t){ { 0.0f, 27.0f, 0.0f } };
    scene->cam_up = (fm_vec3_t){ { 0.0f, 1.0f, 0.0f } };
    scene->clear_color = RGBA32(0, 0, 0, 255);
    scene->near_z = 32.0f;
    scene->far_z = 4096.0f;
}

void kiln_splash_draw3d(void)
{
    if (g_done) return;

    if (!g_xform_ready) {
        kiln_transform_init(&g_xform);
        g_xform_ready = 1;
    }

    /* Assemble: spin down to rest and scale up into frame, both finishing
     * exactly on the beat. */
    const float k = ease_out(g_t / BEAT);
    const float spin = (1.0f - k) * SPIN_TURNS * 6.2831853f;
    const float scale = 0.55f + 0.45f * k;

    g_xform.pos = (fm_vec3_t){ { 0.0f, 19.0f, 0.0f } };
    g_xform.scale = (fm_vec3_t){ { scale, scale, scale } };
    g_xform.rot_axis = (fm_vec3_t){ { 0.0f, 1.0f, 0.0f } };
    g_xform.rot_angle = spin;

    if (!g_model) {
        /* No model: nothing is drawn in 3D and draw2d puts up the rectangle
         * fallback instead. Handled there rather than here because the
         * fallback is 2D and this is the 3D pass. */
        return;
    }

    if (!g_objects_looked_up) {
        g_objects_looked_up = 1;
        g_kiln_obj = t3d_model_get_object(g_model, "kiln");
        g_flame_obj = t3d_model_get_object(g_model, "flame");
        g_plate_obj = t3d_model_get_object(g_model, "plate");
    }

    /* Named objects found: draw body and plate under the settle transform,
     * and the flame under its OWN transform stacked on top of it, so the
     * flame can flicker independently instead of being baked into the
     * mesh. A caller's own model that does not use these three names draws
     * as one rigid piece below — that has always been this module's
     * contract, and a lookup miss must not turn into a blank frame. */
    if (g_kiln_obj || g_flame_obj || g_plate_obj) {
        kiln_transform_push(&g_xform);
        if (g_kiln_obj)  t3d_model_draw_object(g_kiln_obj, NULL);
        if (g_plate_obj) t3d_model_draw_object(g_plate_obj, NULL);
        kiln_transform_pop();

        if (g_flame_obj) {
            if (!g_flame_xform_ready) {
                kiln_transform_init(&g_flame_xform);
                g_flame_xform_ready = 1;
            }
            /* Same rigid placement as the body, plus a small independent
             * scale flicker — the flame breathing in place rather than
             * sliding around, which is what a real flame at this scale
             * mostly does. */
            const float flick = flame_flicker(g_t);
            g_flame_xform.pos = g_xform.pos;
            g_flame_xform.rot_axis = g_xform.rot_axis;
            g_flame_xform.rot_angle = g_xform.rot_angle;
            g_flame_xform.scale = (fm_vec3_t){ { scale * flick,
                                                 scale * (0.92f + 0.08f * flick),
                                                 scale * flick } };
            kiln_transform_push(&g_flame_xform);
            t3d_model_draw_object(g_flame_obj, NULL);
            kiln_transform_pop();
        }
        return;
    }

    kiln_transform_push(&g_xform);
    t3d_model_draw(g_model);
    kiln_transform_pop();
}

void kiln_splash_draw2d(int w, int h)
{
    if (g_done) return;

    /* Fallback wordmark: three bars in the logo's proportions, so a build
     * without the model still shows something deliberate rather than a
     * black screen someone has to debug. */
    if (!g_model) {
        const float k = ease_out(g_t / BEAT);
        const int bw = (int)(120 * k);
        const color_t ink = RGBA32(206, 202, 196, 255);
        const color_t red = RGBA32(176, 26, 32, 255);
        kiln_gui_rect(w / 2 - bw, h / 2 - 24, bw, 8, ink);
        kiln_gui_rect(w / 2 - bw, h / 2 - 8, (int)(bw * 0.6f), 8, red);
        kiln_gui_rect(w / 2 - bw, h / 2 + 8, (int)(bw * 0.8f), 8, red);
    }

    /* The publisher line, fading up after the mark has landed, and lit by
     * the same fire the 3D flame is flickering with — flame_flicker(g_t)
     * drives both, so the glow on the words and the flex of the flame read
     * as one light source rather than an unrelated coincidence.
     *
     * The 2D pass has no lighting (kiln_gui_begin turns depth and shading
     * state off — see kiln_gui.c), so this is the only way "light from
     * the fire reaches the text" can mean anything here: not a real light
     * hitting real geometry, but the text's own colour pulled warmer
     * exactly in time with the flame it is standing next to. */
    if (g_line && g_t >= T_LINE) {
        float a = (g_t - T_LINE) / 0.5f;
        if (a > 1.0f) a = 1.0f;

        float glow = (flame_flicker(g_t) - 0.84f) / 0.32f;
        if (glow < 0.0f) glow = 0.0f;
        if (glow > 1.0f) glow = 1.0f;
        const uint8_t lr = (uint8_t)(190.0f + glow * (255.0f - 190.0f));
        const uint8_t lg = (uint8_t)(186.0f + glow * (214.0f - 186.0f));
        const uint8_t lb = (uint8_t)(180.0f + glow * (130.0f - 180.0f));

        const int len = (int)strlen(g_line);
        kiln_gui_text(w / 2 - len * 4, h / 2 + 52,
                     RGBA32(lr, lg, lb, (uint8_t)(a * 255.0f)),
                     "%s", g_line);
    }

    /* The flash on the beat: one frame at full, then a short decay. Held
     * in frames rather than seconds because it is meant to be a single
     * bright frame followed by a falloff, and at 60 Hz a seconds-based
     * timer rounds that to either nothing or too much. */
    if (g_flash > 0) {
        const uint8_t a = (uint8_t)(220 - (g_flash - 1) * 28);
        kiln_gui_rect(0, 0, w, h, RGBA32(255, 255, 255, a));
        if (++g_flash > 8) g_flash = 0;
    }

    /* Fade to black on the way out, so the caller's first screen can
     * simply begin — it never has to know the splash was there. */
    if (g_t >= T_FADE_OUT) {
        float a = (g_t - T_FADE_OUT) / (T_END - T_FADE_OUT);
        if (a > 1.0f) a = 1.0f;
        kiln_gui_rect(0, 0, w, h, RGBA32(0, 0, 0, (uint8_t)(a * 255.0f)));
    }
}
