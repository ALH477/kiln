/* SPDX-License-Identifier: MIT
 *
 * forge_ent.c — M5: entity placement.
 *
 * An entity here is exactly what a Quake `.map` point entity is and what
 * `kiln_map.c` parses into an `KilnRoomSpawn`: a classname, an origin, an angle,
 * and a handful of key/value epairs that `kiln_dict_set_auto` types on the way in
 * ("0 0 0" is a vec3, "5.5" a float, "5" an int, else a string).
 *
 * ── Why the classnames are a fixed table ───────────────────────────────
 *
 * Because there is no keyboard. A free-text field on an N64 means an on-screen
 * alphabet and a lot of D-pad, for a value that must match a
 * `kiln_map_register_classname` call in the consuming game anyway — so a name
 * typed here that the game does not know is a spawn that silently never
 * appears. A list the game also publishes is both easier to use and impossible
 * to misspell. The same argument tools/mapmaker/src/entity.js makes with its
 * KNOWN_CLASSNAMES.
 *
 * Epairs are the escape hatch, and they are numeric on purpose: `count`,
 * `delay`, `speed`, `target_room` — the arguments that actually vary per
 * placement. Anything needing a string belongs in the game's own table.
 */
#include <stdio.h>
#include "forge.h"

/* Mirrors tools/mapmaker/src/entity.js's KNOWN_CLASSNAMES (ENTITY_PALETTE) and
 * the `info_*` vocabulary the engine's own examples register via
 * kiln_map_register_classname — examples/fps/main.c registers the widest set.
 * Kept short deliberately: a picker is only better than typing while it is
 * short enough to cycle. */
static const char *const CLASSNAMES[FORGE_CLASSNAMES] = {
    "info_player_start",
    "info_enemy",
    "info_heavy",
    "info_health",
    "info_ammo",
    "info_armor",
    "info_npc",
    "info_key_door",
};

/* The numeric epairs an entity can carry. Names, not indices, because the .map
 * carries a name and kiln_dict interns it — an index would have to agree with a
 * table in the consuming game, which is the drift this avoids. */
static const char *const EPAIR_KEYS[FORGE_EPAIRS] = { "count", "delay", "speed" };

const char *forge_ent_classname(int i)
{
    return CLASSNAMES[(i % FORGE_CLASSNAMES + FORGE_CLASSNAMES) % FORGE_CLASSNAMES];
}

const char *forge_ent_epair_key(int i) { return EPAIR_KEYS[i % FORGE_EPAIRS]; }

/* Nearest entity to a world position, within one block. Used to select what the
 * reticle is over, because entities are points and a point cannot be raycast
 * against — the alternative is a picker list, which is worse than pointing. */
static int nearest(const Forge *f, fm_vec3_t at, float max_dist)
{
    int best = -1;
    float bd = max_dist * max_dist;
    for (int i = 0; i < f->ent_count; i++) {
        float dx = f->ents[i].pos.v[0] - at.v[0];
        float dy = f->ents[i].pos.v[1] - at.v[1];
        float dz = f->ents[i].pos.v[2] - at.v[2];
        float d = dx * dx + dy * dy + dz * dz;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

void forge_ent_update(Forge *f, const KilnInput *in)
{
    const float B = (float)KILN_VOXEL_BLOCK_UNITS;

    /* Aim the same way GEO does, so the two modes agree about where you are
     * pointing. The entity goes in the AIR cell in front of the surface — an
     * entity inside a wall is the placement mistake this makes impossible. */
    fm_vec3_t fwd = forge_cam_forward(f);
    kiln_voxel_raycast(&f->world, &f->fly_pos, &fwd, 64.0f * B, &f->aim);

    fm_vec3_t at = { { 0, 0, 0 } };
    if (f->aim.hit) {
        at.v[0] = f->world.offset.v[0] + ((float)f->aim.px + 0.5f) * B;
        at.v[1] = f->world.offset.v[1] + ((float)f->aim.py + 0.5f) * B;
        at.v[2] = f->world.offset.v[2] + ((float)f->aim.pz + 0.5f) * B;
    }

    f->ent_hover = f->aim.hit ? nearest(f, at, B) : -1;

    if ((in->edges & KILN_BTN_A) && f->aim.hit) {
        if (f->ent_hover >= 0) {
            /* Something is already there: move it rather than stacking a second
             * entity on the same cell, which is invisible and then confusing. */
            f->ents[f->ent_hover].pos = at;
            f->ent_sel = f->ent_hover;
        } else if (f->ent_count < FORGE_MAX_ENTS) {
            ForgeEnt *e = &f->ents[f->ent_count];
            memset(e, 0, sizeof *e);
            e->pos = at;
            e->classname = (uint8_t)f->ent_class;
            /* Face the way the camera is facing, in degrees, which is what the
             * .map "angle" epair means. */
            float deg = f->fly_yaw * (180.0f / 3.14159265f);
            while (deg < 0.0f) deg += 360.0f;
            e->angle = (int16_t)deg;
            f->ent_sel = f->ent_count++;
        } else {
            f->ent_full = 1;    /* reported, never silently dropped */
        }
    }

    if ((in->edges & KILN_BTN_B) && f->ent_hover >= 0) {
        /* Swap-with-last removal: order is not meaningful (the .map emitter
         * writes them in array order and nothing reads that order back), and a
         * memmove of 64 structs per delete is work for no property. */
        f->ents[f->ent_hover] = f->ents[--f->ent_count];
        if (f->ent_sel >= f->ent_count) f->ent_sel = f->ent_count - 1;
        f->ent_hover = -1;
    }

    if (in->edges & KILN_BTN_DR) f->ent_class = (f->ent_class + 1) % FORGE_CLASSNAMES;
    if (in->edges & KILN_BTN_DL)
        f->ent_class = (f->ent_class + FORGE_CLASSNAMES - 1) % FORGE_CLASSNAMES;

    /* Edit the selected entity's epairs. C-up/down picks the key, R/Z step the
     * value — R up, Z down, because they are the two buttons not already spoken
     * for in this mode and a value editor needs a pair. */
    if (f->ent_sel >= 0 && f->ent_sel < f->ent_count) {
        ForgeEnt *e = &f->ents[f->ent_sel];
        if (in->edges & KILN_BTN_CU) f->ent_field = (f->ent_field + 1) % FORGE_EPAIRS;
        if (in->edges & KILN_BTN_CD)
            f->ent_field = (f->ent_field + FORGE_EPAIRS - 1) % FORGE_EPAIRS;
        if (in->edges & KILN_BTN_R) e->epair[f->ent_field]++;
        if (in->edges & KILN_BTN_Z && e->epair[f->ent_field] > 0)
            e->epair[f->ent_field]--;
    }
}

void forge_ent_draw3d(Forge *f)
{
    const float B = (float)KILN_VOXEL_BLOCK_UNITS;
    kiln_dd_begin(&f->scene, FORGE_SCREEN_W, FORGE_SCREEN_H);

    for (int i = 0; i < f->ent_count; i++) {
        /* An axis gizmo, not a box: an entity is a point WITH A FACING, and the
         * facing is half of what "did I place this right" means for a spawn.
         * Engine axes — red +X, green +Y up, blue +Z — because the bug class
         * here is a +Z-up convention meeting a +Y-up runtime. */
        kiln_dd_axes(f->ents[i].pos, B * 0.6f);

        /* The angle, drawn as a line so it is legible without reading a number
         * off the HUD one entity at a time. */
        float rad = (float)f->ents[i].angle * (3.14159265f / 180.0f);
        fm_vec3_t tip = {{ f->ents[i].pos.v[0] + fm_sinf(rad) * B,
                           f->ents[i].pos.v[1],
                           f->ents[i].pos.v[2] + fm_cosf(rad) * B }};
        kiln_dd_line(f->ents[i].pos, tip,
                    i == f->ent_sel ? RGBA32(255, 240, 80, 255)
                                    : RGBA32(120, 200, 255, 255));

        /* An index, not the classname. A world-space "info_player_start" is 17
         * characters over a 320-pixel screen and lands on top of the panel that
         * already names it — the panel is where a name belongs, the world is
         * where a POSITION belongs. Same reasoning as thinning CAM's key labels
         * down to the selected one. */
        kiln_dd_text(f->ents[i].pos,
                    i == f->ent_sel ? RGBA32(255, 240, 80, 255)
                                    : RGBA32(160, 200, 230, 255),
                    "e%d", i);
    }

    kiln_dd_end();
}

void forge_ent_draw(Forge *f)
{
    color_t hot = RGBA32(255, 210, 70, 255);
    color_t dim = RGBA32(150, 150, 150, 255);

    kiln_gui_text(6, 72, hot, "place %s", forge_ent_classname(f->ent_class));
    kiln_gui_text(6, 82, f->ent_full ? RGBA32(255, 80, 70, 255) : dim,
                 "ents %d/%d%s", f->ent_count, FORGE_MAX_ENTS,
                 f->ent_full ? " FULL" : "");

    if (f->ent_sel >= 0 && f->ent_sel < f->ent_count) {
        const ForgeEnt *e = &f->ents[f->ent_sel];
        kiln_gui_text(6, 92, dim, "sel %s ang %d",
                     forge_ent_classname(e->classname), e->angle);
        for (int i = 0; i < FORGE_EPAIRS; i++)
            kiln_gui_text(6, 102 + i * 10, i == f->ent_field ? hot : dim,
                         "%c %s %d", i == f->ent_field ? '>' : ' ',
                         forge_ent_epair_key(i), e->epair[i]);
    }
}

/* ── .map emission ─────────────────────────────────────────────────────
 *
 * One point entity per placement, in the dialect kiln_map.c:223 parses (key <=
 * 63 chars, value <= 255). Epairs whose value is zero are OMITTED rather than
 * written as "0": a spawn arg the author never touched should not become a
 * value the game reads, because kiln_dict_get_int's default and an explicit 0
 * are different intentions and only one of them was expressed.
 */
int forge_ent_emit(const Forge *f, char *out, int cap)
{
    int n = 0;
    for (int i = 0; i < f->ent_count && n < cap - 256; i++) {
        const ForgeEnt *e = &f->ents[i];
        n += snprintf(out + n, (size_t)(cap - n),
                      "{\n\"classname\" \"%s\"\n\"origin\" \"%d %d %d\"\n"
                      "\"angle\" \"%d\"\n",
                      forge_ent_classname(e->classname),
                      (int)e->pos.v[0], (int)e->pos.v[1], (int)e->pos.v[2],
                      e->angle);
        for (int k = 0; k < FORGE_EPAIRS; k++)
            if (e->epair[k])
                n += snprintf(out + n, (size_t)(cap - n), "\"%s\" \"%d\"\n",
                              forge_ent_epair_key(k), e->epair[k]);
        n += snprintf(out + n, (size_t)(cap - n), "}\n");
    }
    return n;
}
