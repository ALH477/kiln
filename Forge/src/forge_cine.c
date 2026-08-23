/* SPDX-License-Identifier: MIT
 *
 * forge_cine.c — M7: authoring a camera shot from inside it.
 *
 * Fly to a pose, press A, and that pose becomes a keyframe. Scrub the timeline
 * and the camera flies the curve those keys describe. Save and it comes out as a
 * `static const PMCamKey NAME[]` table in the exact literal syntax a game's
 * own keyframe tables already use.
 *
 * This replaces a loop that already worked and was entirely manual: a
 * downstream game's own cinematic debugger's detached free-fly prints
 * `KEY t ... eye ... look ...` continuously so that any screenshot carries
 * the numbers, and you transcribe them into the table by hand. That is a
 * good loop — it is why the readout is unlatched — but it is one key at a
 * time with no way to see the CURVE the keys imply until the next build.
 *
 * ── The three things that make this worth having ───────────────────────
 *
 * 1. **The same curve.** kiln_camkey_sample is what the runtime flies, what the
 *    overlay draws, what the validator measures, and now what this previews. A
 *    preview of a curve that merely resembles the shipped one would let you tune
 *    against the wrong shape — the same argument kiln_camkey.h makes about the
 *    validator, and it applies harder to an author than to a checker.
 *
 * 2. **Both curves drawn.** The dim line is the straight chord through the keys;
 *    the bright one is what kiln_camkey_sample actually produces. The gap between
 *    them IS the Catmull-Rom overshoot, which is invisible as a table of
 *    coordinates and obvious as a bulge — a shot needing hand-inserted
 *    midpoint keys purely to suppress that overshoot is a real, previously
 *    hard-won case, not a hypothetical one.
 *
 * 3. **The validator runs before the save, not after.** kiln_camlint's hard
 *    failures are facts — duplicate times, a key past the duration, eye == look
 *    (a zero-length view vector that halts the VR4300 several layers from the
 *    table that caused it). An editor that writes a table the game will refuse
 *    has moved the failure two tools away from the person who caused it.
 */
#include <math.h>
#include <stdio.h>
#include "forge.h"

#define CINE_MIN_SPAN 0.05f   /* keys closer than this in time are a duplicate */

/* Insert a key at `t`, keeping the table sorted. Sorted on insert rather than
 * on save because kiln_camkey_sample's bracketing scan assumes it, and a preview
 * that flies an unsorted table would show a curve the game never will. */
static void insert_key(Forge *f, float t, fm_vec3_t eye, fm_vec3_t look)
{
    if (f->key_count >= FORGE_MAX_KEYS) { f->key_full = 1; return; }

    int at = 0;
    while (at < f->key_count && f->keys[at].t < t) at++;

    /* Replace rather than stack when landing on an existing key. Two keys at the
     * same instant is KILN_CAMLINT_ERR_TIME_ORDER, and the reason it is a hard
     * failure is subtle enough to be worth preventing at the source: the
     * bracketing scan walks PAST a zero-span segment, so the duplicate is never
     * flown through — but Catmull-Rom takes its tangent from a key's two
     * NEIGHBOURS, so it still bends the curve either side of the seam while
     * being unreachable itself. Dead data that is not inert. INTAKE shipped with
     * exactly that for months. */
    if (at < f->key_count && f->keys[at].t - t < CINE_MIN_SPAN
                          && t - f->keys[at].t < CINE_MIN_SPAN) {
        f->keys[at].eye = eye;
        f->keys[at].look = look;
        f->key_sel = at;
        return;
    }

    for (int i = f->key_count; i > at; i--) f->keys[i] = f->keys[i - 1];
    f->keys[at].t = t;
    f->keys[at].eye = eye;
    f->keys[at].look = look;
    f->key_count++;
    f->key_sel = at;
}

/* The validator, run over the table as it stands. Called every frame rather
 * than only on save: a red line while you are still editing is a correction,
 * the same line at save time is a rejection. */
void forge_cine_validate(Forge *f)
{
    KilnCamShot shot = {
        .keys = f->keys,
        .key_count = f->key_count,
        .duration = f->cine_duration,
        /* RESOLVED near/far, i.e. what the shot will actually run with — the
         * header is explicit that validating a raw 0 checks a value the
         * renderer never sees. */
        .near_z = f->scene.near_z,
        .far_z = f->scene.far_z,
        .loop = f->cine_loop,
    };

    /* Bound the shot against the level's own extents, so an eye outside the
     * geometry is NOTED. Derived from the world rather than declared, because a
     * constant would be wrong the moment someone builds somewhere else. */
    KilnCamBounds bounds = { .valid = 0 };
    int mn[3], mx[3];
    if (kiln_voxel_bounds(&f->world, mn, mx)) {
        const float B = (float)KILN_VOXEL_BLOCK_UNITS;
        for (int a = 0; a < 3; a++) {
            bounds.mins.v[a] = f->world.offset.v[a] + (float)mn[a] * B;
            bounds.maxs.v[a] = f->world.offset.v[a] + (float)(mx[a] + 1) * B;
        }
        bounds.valid = 1;
    }

    f->cine_err = kiln_camlint(&shot, &bounds, &f->cine_report);
}

void forge_cine_update(Forge *f, const KilnInput *in, float dt)
{
    /* The fly camera is always live in this mode — you author from where you
     * are standing. Scrubbing overrides where it LOOKS, not where you fly. */
    if (!f->cine_playing) forge_cam_update(f, in, dt);

    if (in->edges & KILN_BTN_A) {
        fm_vec3_t fwd = forge_cam_forward(f);
        fm_vec3_t look = f->fly_pos;
        /* The look target sits a fixed distance ahead. A key stores a POINT, not
         * a direction, so the distance matters: too near and the curve's look
         * path whips as the eye moves past it; too far and the shot never
         * converges on a subject. One eighth of the far plane keeps it in
         * proportion to the scene the same way the fly speed does. */
        float d = f->scene.far_z * 0.125f;
        for (int a = 0; a < 3; a++) look.v[a] += fwd.v[a] * d;
        insert_key(f, f->cine_t, f->fly_pos, look);
    }

    if ((in->edges & KILN_BTN_B) && f->key_count > 0 && f->key_sel >= 0) {
        for (int i = f->key_sel; i < f->key_count - 1; i++) f->keys[i] = f->keys[i + 1];
        f->key_count--;
        if (f->key_sel >= f->key_count) f->key_sel = f->key_count - 1;
        f->key_full = 0;
    }

    /* START is save (handled in forge_main), so playback is Z — held, so that
     * letting go leaves you exactly where you were rather than at the end. */
    f->cine_playing = (in->buttons & KILN_BTN_Z) ? 1 : 0;
    if (f->cine_playing) {
        f->cine_t += dt;
        if (f->cine_t > f->cine_duration) f->cine_t = f->cine_loop ? 0.0f
                                                                  : f->cine_duration;
    }

    /* D-left/right scrub, D-up/down step key to key. The same map pm_cine's
     * transport uses, so the two do not have to be remembered separately. */
    if (in->buttons & KILN_BTN_DL) f->cine_t -= dt * 2.0f;
    if (in->buttons & KILN_BTN_DR) f->cine_t += dt * 2.0f;
    if ((in->edges & KILN_BTN_DU) && f->key_count) {
        f->key_sel = (f->key_sel + 1) % f->key_count;
        f->cine_t = f->keys[f->key_sel].t;
    }
    if ((in->edges & KILN_BTN_DD) && f->key_count) {
        f->key_sel = (f->key_sel + f->key_count - 1) % f->key_count;
        f->cine_t = f->keys[f->key_sel].t;
    }
    if (f->cine_t < 0.0f) f->cine_t = 0.0f;
    if (f->cine_t > f->cine_duration) f->cine_t = f->cine_duration;

    /* R lengthens the shot, L+R is the mode chord so L alone shortens it. */
    if ((in->edges & KILN_BTN_R) && !(in->buttons & KILN_BTN_L))
        f->cine_duration += 1.0f;
    if ((in->edges & KILN_BTN_L) && !(in->buttons & KILN_BTN_R)
        && f->cine_duration > 1.0f)
        f->cine_duration -= 1.0f;

    /* ── Frame the frustum on the subject ──────────────────────────────
     *
     * The editing camera's far plane is 6000 units because the addressable world
     * is that big. A shot in a 512-unit room inheriting it draws a frustum whose
     * edges leave the screen immediately — noise, and worse than noise because it
     * hides the one thing the drawing is for: whether the far plane cuts the
     * subject. It also made kiln_camlint's SUBJECT_CUT check vacuous, since
     * nothing is ever past a far plane twelve times the room's size.
     *
     * So a shot resolves its own near/far from the keys' own subject distance,
     * which is what a real shot's PMDemoShot fields carry and what
     * forge_cine_emit writes into the exported comment. Four times the farthest
     * subject leaves room for the curve to swing out without clipping it. */
    if (f->key_count > 0) {
        float worst = 0.0f;
        for (int i = 0; i < f->key_count; i++) {
            float dx = f->keys[i].look.v[0] - f->keys[i].eye.v[0];
            float dy = f->keys[i].look.v[1] - f->keys[i].eye.v[1];
            float dz = f->keys[i].look.v[2] - f->keys[i].eye.v[2];
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d > worst) worst = d;
        }
        if (worst > 0.0f) {
            f->scene.far_z = worst * 4.0f;
            if (f->scene.far_z < 200.0f) f->scene.far_z = 200.0f;
        }
    }

    forge_cine_validate(f);
}

/* Fly the shot's own camera, so the 3D pass shows what the shot shows. Returns
 * 1 when it took the camera — mirroring pm_cine_override_camera's shape, which
 * lets the call site stay unconditional. */
int forge_cine_override_camera(Forge *f)
{
    if (f->key_count < 2) return 0;
    fm_vec3_t eye, look;
    kiln_camkey_sample(f->keys, f->key_count, f->cine_loop, f->cine_t, &eye, &look);
    f->scene.cam_pos = eye;
    f->scene.cam_target = look;
    f->scene.cam_up = (fm_vec3_t){{ 0.0f, 1.0f, 0.0f }};
    kiln_scene_update(&f->scene);
    return 1;
}

void forge_cine_draw3d(Forge *f)
{
    if (f->key_count == 0) return;
    kiln_dd_begin(&f->scene, FORGE_SCREEN_W, FORGE_SCREEN_H);

    /* The chord: straight lines between keys. Dim, because it is the reference,
     * not the answer. */
    for (int i = 0; i + 1 < f->key_count; i++)
        kiln_dd_line(f->keys[i].eye, f->keys[i + 1].eye, RGBA32(90, 90, 110, 255));

    /* The flown curve, sampled from the same function the runtime calls. The GAP
     * between this and the chord is the overshoot. */
    if (f->key_count >= 2) {
        const int STEPS = 64;
        fm_vec3_t prev_e, prev_l;
        kiln_camkey_sample(f->keys, f->key_count, f->cine_loop, 0.0f, &prev_e, &prev_l);
        for (int s = 1; s <= STEPS; s++) {
            float t = f->cine_duration * (float)s / (float)STEPS;
            fm_vec3_t e, l;
            kiln_camkey_sample(f->keys, f->key_count, f->cine_loop, t, &e, &l);
            kiln_dd_line(prev_e, e, RGBA32(120, 255, 160, 255));
            /* The LOOK path overshoots the same way and is usually what makes a
             * camera feel drunk — drawn in amber, as the overlay does. */
            kiln_dd_line(prev_l, l, RGBA32(255, 170, 60, 255));
            prev_e = e; prev_l = l;
        }
    }

    for (int i = 0; i < f->key_count; i++) {
        kiln_dd_axes(f->keys[i].eye, 12.0f);
        /* The sightline from each eye key to its own target. Where one crosses a
         * wall, the shot opens inside geometry. */
        kiln_dd_line(f->keys[i].eye, f->keys[i].look,
                    i == f->key_sel ? RGBA32(255, 240, 80, 255)
                                    : RGBA32(80, 120, 160, 255));
        /* Only the selected key is labelled. Thirty-two world-space labels on a
         * 320x240 screen overlap each other and the status block, and a diagram
         * you cannot read is not a diagram — the others are identifiable by
         * their gizmos and their order along the curve. */
        if (i == f->key_sel)
            kiln_dd_text(f->keys[i].eye, RGBA32(255, 240, 80, 255),
                        "k%d %.1f", i, (double)f->keys[i].t);
    }

    /* The frustum the shot actually resolved, drawn from wherever you are
     * standing — so "the far plane cuts the room" is a picture rather than two
     * numbers.
     *
     * Two conditions, and the second was learned from a capture. Not while the
     * shot drives the camera, obviously. And not while the viewer is INSIDE it
     * either: kiln_debugdraw.h says outright that a frustum drawn from inside
     * itself is a full-screen X, and at a quarter of the far plane you are close
     * enough for its edges to sweep straight past you — which is what the first
     * CAM capture showed, purple lines fanning across everything and hiding the
     * geometry the frustum is supposed to be measured against.
     *
     * When it is hidden the HUD says so, because a missing overlay and an overlay
     * with nothing to show are the same picture. */
    f->cine_frustum_hidden = 0;
    if (!f->cine_playing && f->key_count >= 2) {
        fm_vec3_t eye, look;
        kiln_camkey_sample(f->keys, f->key_count, f->cine_loop, f->cine_t, &eye, &look);
        float dx = f->fly_pos.v[0] - eye.v[0];
        float dy = f->fly_pos.v[1] - eye.v[1];
        float dz = f->fly_pos.v[2] - eye.v[2];
        if (sqrtf(dx * dx + dy * dy + dz * dz) > f->scene.far_z * 0.25f)
            kiln_dd_frustum(eye, look, f->scene.fov_deg, 4.0f / 3.0f,
                           f->scene.near_z, f->scene.far_z,
                           RGBA32(200, 120, 255, 200));
        else
            f->cine_frustum_hidden = 1;
    }

    kiln_dd_end();
}

void forge_cine_draw(Forge *f)
{
    color_t hot = RGBA32(255, 210, 70, 255);
    color_t dim = RGBA32(150, 150, 150, 255);
    color_t bad = RGBA32(255, 80, 70, 255);

    kiln_gui_text(6, 72, hot, "t %.2f of %.1f  keys %d%s  %s",
                 (double)f->cine_t, (double)f->cine_duration, f->key_count,
                 f->key_full ? " FULL" : "", f->cine_playing ? "PLAY" : "");

    /* The validator's verdict, named. "invalid" would be a fact the author then
     * has to bisect; the flag's name points at the rule. */
    if (f->cine_err) {
        int shown = 0;
        for (int b = 0; b < 32 && shown < 3; b++)
            if (f->cine_err & (1u << b))
                kiln_gui_text(6, 82 + shown++ * 10, bad, "ERR %s%s",
                             kiln_camlint_err_name(1u << b),
                             f->cine_report.bad_key >= 0 ? "" : "");
        if (f->cine_report.bad_key >= 0)
            kiln_gui_text(6, 82 + shown * 10, bad, "at key %d",
                         f->cine_report.bad_key);
    } else if (f->key_count >= 2) {
        kiln_gui_text(6, 82, dim, "ok  overshoot %.2f k%d  hitch %.1f",
                     (double)f->cine_report.overshoot,
                     f->cine_report.overshoot_seg,
                     (double)f->cine_report.speed_ratio);
        if (f->cine_frustum_hidden)
            kiln_gui_text(6, 92, dim, "frustum hidden - fly back to see it");
    }

    /* The timeline: duration as a bar, keys as ticks, the playhead on top. */
    const int bx0 = 6, bx1 = FORGE_SCREEN_W - 7, by = FORGE_SCREEN_H - 44;
    kiln_gui_rect(bx0, by, bx1 - bx0, 3, RGBA32(70, 70, 80, 255));
    for (int i = 0; i < f->key_count; i++) {
        float fr = f->cine_duration > 0.0f ? f->keys[i].t / f->cine_duration : 0.0f;
        int x = bx0 + (int)((float)(bx1 - bx0) * fr);
        kiln_gui_rect(x, by - 3, 1, 9, i == f->key_sel ? hot : RGBA32(220, 220, 220, 255));
    }
    float pf = f->cine_duration > 0.0f ? f->cine_t / f->cine_duration : 0.0f;
    kiln_gui_rect(bx0 + (int)((float)(bx1 - bx0) * pf), by - 5, 1, 13,
                 RGBA32(120, 255, 140, 255));

    /* The pose readout, continuous and unlatched, in the order the table wants
     * it. Kept even though this mode WRITES the table: a screenshot of the
     * editor should still carry the numbers, which is what makes the loop work
     * from a capture and not only from a controller. Alphanumeric plus '.' and
     * '-' only — FONT_BUILTIN_DEBUG_MONO has no '@' and renders it as '0'. */
    if (f->key_sel >= 0 && f->key_sel < f->key_count) {
        const KilnCamKey *k = &f->keys[f->key_sel];
        kiln_gui_text(6, FORGE_SCREEN_H - 40, dim,
                     "KEY t %.2f eye %.0f %.0f %.0f look %.0f %.0f %.0f",
                     (double)k->t,
                     (double)k->eye.v[0], (double)k->eye.v[1], (double)k->eye.v[2],
                     (double)k->look.v[0], (double)k->look.v[1], (double)k->look.v[2]);
    }
}

/* ── Export ────────────────────────────────────────────────────────────
 *
 * The exact literal syntax a game's own PMCamKey tables use, including the
 * DOUBLE BRACE — fm_vec3_t wraps a `float v[3]`, so each vector is
 * {{ x, y, z }} and a single brace does not compile. Emitting text the target
 * project can paste unchanged is the whole reason this is ASCII rather than a
 * blob: the host side is a copy, not a decoder.
 */
int forge_cine_emit(const Forge *f, char *out, int cap)
{
    int n = snprintf(out, (size_t)cap,
        "/* Generated by Forge's CAM mode. Paste into the shot's own .c.\n"
        " *\n"
        " * Validated at authoring time by kiln_camlint (the same rules\n"
        " * ./dev cine-lint runs), so the hard failures are already ruled out:\n"
        " * ordered unique times, no key past the duration, no eye == look, and\n"
        " * the subject inside the far plane it was authored against.\n"
        " *\n"
        " * near_z %.1f  far_z %.1f  -- the frustum these keys were framed for.\n"
        " * A shot that declares neither falls back to PM_SHOT_NEAR_Z /\n"
        " * PM_SHOT_FAR_Z, NOT to the scene's current values, so carry them over.\n"
        " */\n"
        "static const PMCamKey FORGE_KEYS[] = {\n",
        (double)f->scene.near_z, (double)f->scene.far_z);

    for (int i = 0; i < f->key_count && n < cap - 160; i++) {
        const KilnCamKey *k = &f->keys[i];
        n += snprintf(out + n, (size_t)(cap - n),
                      "    { %5.2ff, {{ %7.1ff, %7.1ff, %7.1ff }},"
                      " {{ %7.1ff, %7.1ff, %7.1ff }} },\n",
                      (double)k->t,
                      (double)k->eye.v[0], (double)k->eye.v[1], (double)k->eye.v[2],
                      (double)k->look.v[0], (double)k->look.v[1], (double)k->look.v[2]);
    }

    n += snprintf(out + n, (size_t)(cap - n),
                  "};\n\n"
                  "/* .duration = %.1ff, .key_count = %d, .loop = %d */\n",
                  (double)f->cine_duration, f->key_count, f->cine_loop);
    return n;
}
