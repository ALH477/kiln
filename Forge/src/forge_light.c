/* SPDX-License-Identifier: MIT
 *
 * forge_light.c — M6: the light rig and the fog, aimed while looking at them.
 *
 * Four fields on KilnScene and nothing more, because that is all this engine has:
 * at most 4 of Tiny3D's 7 directional lights, no point lights, no spots, no
 * shadows (CLAUDE.md, "What genuinely does not exist"). So there is no light
 * editor to build — there is a key direction, a fill direction, an ambient level
 * and a fog range, and the only hard part is that all four are judgements about
 * a picture rather than numbers.
 *
 * Which is the entire argument for doing it here. `kiln_scene_set_fog` is
 * per-pixel in the blender and costs a scene that already fogs almost nothing,
 * but whether a fog range makes distance READ or just makes the level grey is
 * not answerable from a host preview at a different resolution and gamma.
 *
 * ── Do not install a rig's brightness ──────────────────────────────────
 *
 * The one trap this mode has to avoid teaching people to fall into: a
 * generator whose bake() multiplies a fixture rig into its own vertex
 * colours (see the n64-modeling skill's "Is the model's light already IN
 * it?" section) needs the runtime to light it close to unity, or it
 * double-darkens — which is exactly what "the room reads as unlit" turns
 * out to be for content built that way. Such a generator's own bake-rig
 * should feed the runtime only its fixtures' DIRECTIONS, never its
 * intensities. A Forge level's geometry carries a per-face shade and no
 * baked rig, so it wants real lighting; a generated header describing
 * pre-baked content should say so in a comment rather than leaving the
 * next reader to find out.
 */
#include <stdio.h>
#include "forge.h"

#define LIGHT_STEP  0.08f    /* radians per press */

static void aim_from_angles(fm_vec3_t *dir, float yaw, float pitch)
{
    float cp = fm_cosf(pitch);
    dir->v[0] = fm_sinf(yaw) * cp;
    dir->v[1] = fm_sinf(pitch);
    dir->v[2] = fm_cosf(yaw) * cp;
}

void forge_light_apply(Forge *f)
{
    KilnScene *s = &f->scene;

    aim_from_angles(&s->light_dir, f->key_yaw, f->key_pitch);
    s->light_color[0] = s->light_color[1] = s->light_color[2] = f->key_level;
    s->light_color[3] = 255;

    /* Light 1 is `lights[0]` — the header's own note: "Index i here is light
     * i+1", light 0 being the named fields above. Getting that off by one puts
     * the fill light where the key is and looks like the fill doing nothing. */
    aim_from_angles(&s->lights[0].dir, f->fill_yaw, f->fill_pitch);
    s->lights[0].color[0] = s->lights[0].color[1] = s->lights[0].color[2] = f->fill_level;
    s->lights[0].color[3] = 255;
    s->light_count = 1;

    s->ambient[0] = s->ambient[1] = s->ambient[2] = f->ambient;
    s->ambient[3] = 255;

    if (f->fog_on) {
        /* Fog colour tracks the clear colour by default. Matching the horizon is
         * what makes far geometry end in haze rather than at a visible edge —
         * and with no skydome in an editor, the clear colour IS the horizon. */
        kiln_scene_set_fog(s, s->clear_color, f->fog_near, f->fog_far);
    } else {
        kiln_scene_disable_fog(s);
    }
}

void forge_light_update(Forge *f, const KilnInput *in)
{
    /* C-up/down selects the field, D-pad edits it. One selector and one editor,
     * rather than a chord per field: there are seven values here and seven
     * chords is a manual. */
    if (in->edges & KILN_BTN_CU)
        f->light_field = (f->light_field + FORGE_LIGHT_FIELDS - 1) % FORGE_LIGHT_FIELDS;
    if (in->edges & KILN_BTN_CD)
        f->light_field = (f->light_field + 1) % FORGE_LIGHT_FIELDS;

    int dx = 0, dy = 0;
    static int rep;
    if (in->buttons & (KILN_BTN_DU | KILN_BTN_DD | KILN_BTN_DL | KILN_BTN_DR)) {
        if (rep == 0 || rep > 6) {
            if (in->buttons & KILN_BTN_DL) dx = -1;
            if (in->buttons & KILN_BTN_DR) dx = +1;
            if (in->buttons & KILN_BTN_DD) dy = -1;
            if (in->buttons & KILN_BTN_DU) dy = +1;
        }
        rep++;
    } else {
        rep = 0;
    }
    int step = (dx ? dx : dy);

    switch (f->light_field) {
    case FORGE_LF_KEY_DIR:
        f->key_yaw += (float)dx * LIGHT_STEP;
        f->key_pitch += (float)dy * LIGHT_STEP;
        break;
    case FORGE_LF_KEY_LEVEL:
        f->key_level = (uint8_t)((int)f->key_level + step * 8 < 0 ? 0
                                : ((int)f->key_level + step * 8 > 255 ? 255
                                : (int)f->key_level + step * 8));
        break;
    case FORGE_LF_FILL_DIR:
        f->fill_yaw += (float)dx * LIGHT_STEP;
        f->fill_pitch += (float)dy * LIGHT_STEP;
        break;
    case FORGE_LF_FILL_LEVEL:
        f->fill_level = (uint8_t)((int)f->fill_level + step * 8 < 0 ? 0
                                 : ((int)f->fill_level + step * 8 > 255 ? 255
                                 : (int)f->fill_level + step * 8));
        break;
    case FORGE_LF_AMBIENT:
        f->ambient = (uint8_t)((int)f->ambient + step * 8 < 0 ? 0
                              : ((int)f->ambient + step * 8 > 255 ? 255
                              : (int)f->ambient + step * 8));
        break;
    case FORGE_LF_FOG_NEAR:
        f->fog_near += (float)step * 32.0f;
        if (f->fog_near < 0.0f) f->fog_near = 0.0f;
        break;
    case FORGE_LF_FOG_FAR:
        f->fog_far += (float)step * 64.0f;
        /* far must stay ahead of near, or the blender's fog factor inverts and
         * the scene reads as fogged where it is nearest — a picture that looks
         * like a depth bug rather than a range one. */
        if (f->fog_far < f->fog_near + 32.0f) f->fog_far = f->fog_near + 32.0f;
        break;
    default: break;
    }

    if (in->edges & KILN_BTN_A) f->fog_on = !f->fog_on;

    /* R cycles the clear colour through a few plausible horizons. Not a full
     * colour picker: the clear colour's job here is to be the fog colour, and
     * three defensible choices beat an RGB triple edited eight units at a time. */
    if (in->edges & KILN_BTN_R) {
        f->clear_idx = (f->clear_idx + 1) % 4;
        static const color_t CLEARS[4] = {
            { .r = 18,  .g = 20,  .b = 26,  .a = 255 },   /* night      */
            { .r = 40,  .g = 44,  .b = 56,  .a = 255 },   /* dusk       */
            { .r = 96,  .g = 104, .b = 120, .a = 255 },   /* overcast   */
            { .r = 8,   .g = 6,   .b = 8,   .a = 255 },   /* black box  */
        };
        f->scene.clear_color = CLEARS[f->clear_idx];
    }

    forge_light_apply(f);
}

void forge_light_draw(Forge *f)
{
    static const char *const NAMES[FORGE_LIGHT_FIELDS] = {
        "key dir", "key level", "fill dir", "fill level",
        "ambient", "fog near", "fog far",
    };
    char val[FORGE_LIGHT_FIELDS][32];
    snprintf(val[FORGE_LF_KEY_DIR],   32, "%.2f %.2f", (double)f->key_yaw, (double)f->key_pitch);
    snprintf(val[FORGE_LF_KEY_LEVEL], 32, "%d", f->key_level);
    snprintf(val[FORGE_LF_FILL_DIR],  32, "%.2f %.2f", (double)f->fill_yaw, (double)f->fill_pitch);
    snprintf(val[FORGE_LF_FILL_LEVEL],32, "%d", f->fill_level);
    snprintf(val[FORGE_LF_AMBIENT],   32, "%d", f->ambient);
    snprintf(val[FORGE_LF_FOG_NEAR],  32, "%.0f", (double)f->fog_near);
    snprintf(val[FORGE_LF_FOG_FAR],   32, "%.0f", (double)f->fog_far);

    color_t hot = RGBA32(255, 210, 70, 255), dim = RGBA32(150, 150, 150, 255);
    kiln_gui_text(6, 72, hot, "fog %s   clear %d", f->fog_on ? "on" : "off", f->clear_idx);
    for (int i = 0; i < FORGE_LIGHT_FIELDS; i++)
        kiln_gui_text(6, 84 + i * 10, i == f->light_field ? hot : dim,
                     "%c %-11s %s", i == f->light_field ? '>' : ' ',
                     NAMES[i], val[i]);
    /* No help line here: forge_hud.c prints the per-mode help for every mode
     * at the bottom of the screen, and this printed the same six bindings a
     * second time, higher up. Two copies of one string is two things to keep
     * in step with Forge/src/forge_binds.def, and the panel is more useful
     * showing the values than repeating the controls. */
}

void forge_light_draw3d(Forge *f)
{
    /* The two light directions, drawn from the camera as rays so an aim is a
     * picture. A direction is otherwise two numbers whose effect you infer from
     * the shading, which is the inference this mode exists to remove. */
    kiln_dd_begin(&f->scene, FORGE_SCREEN_W, FORGE_SCREEN_H);
    const float L = 3.0f * (float)KILN_VOXEL_BLOCK_UNITS;
    fm_vec3_t at = f->fly_pos;
    fm_vec3_t fwd = forge_cam_forward(f);
    for (int a = 0; a < 3; a++) at.v[a] += fwd.v[a] * L * 2.0f;

    fm_vec3_t k = at, fl = at;
    for (int a = 0; a < 3; a++) {
        k.v[a]  -= f->scene.light_dir.v[a] * L;
        fl.v[a] -= f->scene.lights[0].dir.v[a] * L;
    }
    kiln_dd_line(k, at, RGBA32(255, 240, 180, 255));
    kiln_dd_text(k, RGBA32(255, 240, 180, 255), "key");
    kiln_dd_line(fl, at, RGBA32(140, 180, 255, 255));
    kiln_dd_text(fl, RGBA32(140, 180, 255, 255), "fill");
    kiln_dd_end();
}

/* ── Export ────────────────────────────────────────────────────────────
 *
 * A generated header, prefixed FORGE_<LEVEL>_ because a generated header is a
 * NAMESPACE and not just a file: two independent generators emitting the same
 * macro name for two different things is a real defect class (see
 * CLAUDE.md's "Two generators must not publish the same macro name") — which
 * room a shared unprefixed macro described has come down to include order,
 * silently, behind a warning nobody saw.
 */
int forge_light_emit(const Forge *f, char *out, int cap)
{
    return snprintf(out, (size_t)cap,
        "/* SPDX-License-Identifier: MIT\n"
        " *\n"
        " * Generated by Forge's LIGHT mode. Do not hand-edit: re-export instead.\n"
        " *\n"
        " * These are DIRECTIONS AND LEVELS for a runtime rig, i.e. the geometry\n"
        " * they light must NOT already have a rig baked into its vertex colours.\n"
        " * A Forge level carries only a per-face shade, so it wants real lighting.\n"
        " * A room whose generator already bakes a fixture rig into its own\n"
        " * vertex colours is the opposite case, and installing this rig's\n"
        " * brightness there double-darkens it instead of lighting it.\n"
        " */\n"
        "#ifndef FORGE_%s_LIGHT_H\n"
        "#define FORGE_%s_LIGHT_H\n"
        "\n"
        "#define FORGE_%s_KEY_DIR      {{ %.4ff, %.4ff, %.4ff }}\n"
        "#define FORGE_%s_KEY_LEVEL    %d\n"
        "#define FORGE_%s_FILL_DIR     {{ %.4ff, %.4ff, %.4ff }}\n"
        "#define FORGE_%s_FILL_LEVEL   %d\n"
        "#define FORGE_%s_AMBIENT      %d\n"
        "#define FORGE_%s_FOG_ENABLED  %d\n"
        "#define FORGE_%s_FOG_NEAR     %.1ff\n"
        "#define FORGE_%s_FOG_FAR      %.1ff\n"
        "#define FORGE_%s_CLEAR_RGB    { %d, %d, %d }\n"
        "\n"
        "#endif\n",
        FORGE_LEVEL_NAME, FORGE_LEVEL_NAME,
        FORGE_LEVEL_NAME, (double)f->scene.light_dir.v[0],
        (double)f->scene.light_dir.v[1], (double)f->scene.light_dir.v[2],
        FORGE_LEVEL_NAME, f->key_level,
        FORGE_LEVEL_NAME, (double)f->scene.lights[0].dir.v[0],
        (double)f->scene.lights[0].dir.v[1], (double)f->scene.lights[0].dir.v[2],
        FORGE_LEVEL_NAME, f->fill_level,
        FORGE_LEVEL_NAME, f->ambient,
        FORGE_LEVEL_NAME, f->fog_on,
        FORGE_LEVEL_NAME, (double)f->fog_near,
        FORGE_LEVEL_NAME, (double)f->fog_far,
        FORGE_LEVEL_NAME, f->scene.clear_color.r, f->scene.clear_color.g,
        f->scene.clear_color.b);
}
