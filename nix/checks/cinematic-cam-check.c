/* SPDX-License-Identifier: MIT
 *
 * examples/cinematic-demo's camera, flown on the host against its own cast.
 *
 * The shots used to be authored for a world sixty times smaller than the
 * models drawn into it, and every camera sat inside a model or a wall. That
 * built, linked and booted to a screen of flat colour. Finding it needed no
 * emulator: the cast's positions are pure functions of time (cine_script.h)
 * and the camera is a set of kiln_camkey tables (cine_shots.h), so both can be
 * sampled here across the whole loop and compared.
 *
 *   camlint     kiln_camlint over every shot: no hard failures, no eye
 *               outside the hangar, and shots that tile the loop exactly
 *   clearance   at every 1/30 s the flown eye stays clear of the walls, floor
 *               and ceiling, the ship, the pad, every actor (its box at any
 *               yaw) and both crate stacks, by more than 1.5x the near plane
 *   framing     the look point stays more than 12 units in front of the eye
 *               and inside the far plane
 *   negative    the same flight over a copy of the shots with one eye moved
 *               into the Interceptor must FAIL. A clearance test only ever
 *               seen to pass might not be measuring anything.
 *
 * It also prints the bounds cine_script.h states, so the nix script can diff
 * them against the glTF each ROM model was converted from.
 */
#include <kiln_camlint.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cine_shots.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

typedef struct { fm_vec3_t mn, mx; } Box;

/* Distance from p to a box: the euclidean gap outside, minus the depth inside. */
static float box_gap(fm_vec3_t p, Box b)
{
    float out2 = 0.0f, in = -1e9f;
    for (int k = 0; k < 3; k++) {
        float d = 0.0f;
        if (p.v[k] < b.mn.v[k]) d = b.mn.v[k] - p.v[k];
        else if (p.v[k] > b.mx.v[k]) d = p.v[k] - b.mx.v[k];
        out2 += d * d;
        const float lo = p.v[k] - b.mn.v[k], hi = b.mx.v[k] - p.v[k];
        const float depth = -(lo < hi ? lo : hi);
        if (depth > in) in = depth;
    }
    return out2 > 0.0f ? sqrtf(out2) : in;
}

/* The box holding an actor's model at ANY yaw: XZ half-extent is the bounds'
 * circumscribed radius. */
static Box actor_box(fm_vec3_t pos, const CineBounds *b)
{
    float r = 0.0f;
    for (int sx = 0; sx < 2; sx++)
        for (int sz = 0; sz < 2; sz++) {
            const float x = sx ? b->mx[0] : b->mn[0], z = sz ? b->mx[2] : b->mn[2];
            if (x * x + z * z > r) r = x * x + z * z;
        }
    r = sqrtf(r) * CINE_UNITS_PER_M;
    return (Box){ {{ pos.v[0] - r, pos.v[1] + b->mn[1] * CINE_UNITS_PER_M, pos.v[2] - r }},
                  {{ pos.v[0] + r, pos.v[1] + b->mx[1] * CINE_UNITS_PER_M, pos.v[2] + r }} };
}

static Box ship_box(void)
{
    /* Drawn turned by pi about Y, so x and z swap sign. */
    const CineBounds *b = &CINE_B_SHIP;
    const float y0 = cine_stand_y(b, CINE_PAD_TOP), m = CINE_UNITS_PER_M;
    return (Box){ {{ -b->mx[0] * m, y0 + b->mn[1] * m, -b->mx[2] * m }},
                  {{ -b->mn[0] * m, y0 + b->mx[1] * m, -b->mn[2] * m }} };
}

typedef struct { float worst; float at; const char *what; } Clear;

static void note(Clear *c, float gap, float t, const char *what)
{
    if (gap < c->worst) { c->worst = gap; c->at = t; c->what = what; }
}

/* Worst clearance over the whole loop for a set of shots. */
static Clear fly(const CineShot *shots, int n, int check_framing)
{
    Clear c = { 1e9f, 0.0f, "nothing" };
    const Box ship = ship_box();
    const Box pad = {{{ -CINE_PAD_HALF, 0.0f, -CINE_PAD_HALF }}, {{ CINE_PAD_HALF, CINE_PAD_TOP, CINE_PAD_HALF }}};
    const float s = CINE_CRATE_HALF;

    for (int f = 0; f < (int)(CINE_LOOP_T * 30.0f); f++) {
        const float t = (float)f / 30.0f;
        fm_vec3_t eye, look;
        cine_camera(shots, n, t, &eye, &look);

        note(&c, eye.v[0] + CINE_WALL_IN, t, "the -X wall");
        note(&c, CINE_WALL_IN - eye.v[0], t, "the +X wall");
        note(&c, eye.v[2] + CINE_WALL_IN, t, "the back wall");
        note(&c, CINE_WALL_IN - eye.v[2], t, "the front wall");
        note(&c, CINE_CEIL_Y - eye.v[1], t, "the ceiling");
        note(&c, eye.v[1] - CINE_FLOOR_Y, t, "the floor");
        note(&c, box_gap(eye, ship), t, "the Interceptor");
        note(&c, box_gap(eye, pad), t, "the pad");

        /* A stack stands full height until it is bumped; after that, one crate
         * high over the whole patch of floor a toppled crate can reach. The
         * box is generous because the physics that decides where they land
         * does not run here. */
        const Box stack_a = {{{ CINE_STACK_A_X - 4 * s, 0.0f, CINE_STACK_A_Z - 4 * s }},
                             {{ CINE_STACK_A_X + 4 * s, t < CINE_BUMP_A_T ? 6 * s : 2 * s, CINE_WALL_IN }}};
        const Box stack_b = {{{ -CINE_WALL_IN, 0.0f, CINE_STACK_B_Z - 4 * s }},
                             {{ CINE_STACK_B_X + 4 * s, t < CINE_BUMP_B_T ? 4 * s : 2 * s, CINE_STACK_B_Z + 10 * s }}};
        note(&c, box_gap(eye, stack_a), t, "crate stack A");
        note(&c, box_gap(eye, stack_b), t, "crate stack B");

        const CinePose g = cine_goblin(t);
        note(&c, box_gap(eye, actor_box(g.pos, &CINE_B_GOBLIN)), t, "the goblin");
        for (int i = 0; i < 2; i++) {
            const CinePose a = cine_alien(i, t, g.pos);
            note(&c, box_gap(eye, actor_box(a.pos, &CINE_B_ALIEN)), t, i ? "the late alien" : "the lead alien");
            /* hangar.map's info_droid radius and phases: 24 at 0 and 180. */
            const CinePose d = cine_droid(24.0f, (float)i * CINE_PI, t);
            note(&c, box_gap(eye, actor_box(d.pos, &CINE_B_DROID)), t, i ? "droid 1" : "droid 0");
        }

        if (check_framing) {
            fm_vec3_t v;
            fm_vec3_sub(&v, &look, &eye);
            const float dist = fm_vec3_len(&v);
            CHECK(dist > 12.0f, "t=%.2f: look point only %.1f units from the eye", t, dist);
            CHECK(dist < CINE_FAR_Z, "t=%.2f: look point %.1f beyond the far plane", t, dist);
        }
    }
    return c;
}

static void bounds_line(const char *name, const CineBounds *b)
{
    printf("BOUNDS %s %.3f %.3f %.3f %.3f %.3f %.3f\n", name,
           b->mn[0], b->mn[1], b->mn[2], b->mx[0], b->mx[1], b->mx[2]);
}

int main(void)
{
    const int n = CINE_SHOT_COUNT;
    const KilnCamBounds room = { {{ -CINE_WALL_IN, CINE_FLOOR_Y, -CINE_WALL_IN }},
                                 {{  CINE_WALL_IN, CINE_CEIL_Y,   CINE_WALL_IN }}, 1 };

    /* ── camlint, per shot, and the shots tile the loop ── */
    CHECK(CINE_SHOTS[0].start == 0.0f, "the first shot does not start at 0");
    for (int i = 0; i < n; i++) {
        const CineShot *s = &CINE_SHOTS[i];
        const float end = (i + 1 < n) ? CINE_SHOTS[i + 1].start : CINE_LOOP_T;
        CHECK(s->start + s->dur == end, "shot %d runs %.2f..%.2f but the next starts at %.2f",
              i, s->start, s->start + s->dur, end);
        const KilnCamShot shot = { s->keys, s->n, s->dur, CINE_NEAR_Z, CINE_FAR_Z, 0 };
        KilnCamReport rep;
        const uint32_t err = kiln_camlint(&shot, &room, &rep);
        printf("shot %d %-14s err 0x%x note 0x%x overshoot %.2f speed %.1f..%.1f subject %.1f tail %.2f\n",
               i, s->title ? s->title : "(untitled)", (unsigned)err, (unsigned)rep.note,
               rep.overshoot, rep.speed_min, rep.speed_max, rep.subject_dist, rep.tail);
        for (int b = 0; b < KILN_CAMLINT_ERR_COUNT; b++)
            CHECK(!(err & (1u << b)), "shot %d: camlint %s (key %d)", i,
                  kiln_camlint_err_name(1u << b), rep.bad_key);
        CHECK(!(rep.note & KILN_CAMLINT_NOTE_OUTSIDE), "shot %d: key %d's eye is outside the hangar",
              i, rep.outside_key);
        CHECK(s->keys[s->n - 1].t == s->dur, "shot %d: its last key is not at its end, so it holds", i);
    }

    /* ── clearance and framing ── */
    const Clear c = fly(CINE_SHOTS, n, 1);
    printf("clearance: worst %.2f units at t=%.2f, from %s (need > %.1f)\n",
           c.worst, c.at, c.what, CINE_NEAR_Z * 1.5f);
    CHECK(c.worst > CINE_NEAR_Z * 1.5f, "t=%.2f: the eye is %.2f from %s", c.at, c.worst, c.what);

    /* ── negative: the same flight must see an eye put inside the ship ── */
    CineShot broken_shots[16];
    KilnCamKey broken_keys[16];
    CHECK(n <= 16 && CINE_SHOTS[2].n <= 16, "tables too big for the negative copy");
    memcpy(broken_shots, CINE_SHOTS, sizeof(CineShot) * (size_t)n);
    memcpy(broken_keys, CINE_SHOTS[2].keys, sizeof(KilnCamKey) * (size_t)CINE_SHOTS[2].n);
    broken_keys[0].eye = (fm_vec3_t){{ 0.0f, cine_stand_y(&CINE_B_SHIP, CINE_PAD_TOP) + 2.0f, 0.0f }};
    broken_shots[2].keys = broken_keys;
    const Clear bad = fly(broken_shots, n, 0);
    printf("negative: worst %.2f at t=%.2f, from %s\n", bad.worst, bad.at, bad.what);
    CHECK(bad.worst < 0.0f && strcmp(bad.what, "the Interceptor") == 0,
          "an eye inside the Interceptor was not caught");

    bounds_line("interceptor", &CINE_B_SHIP);
    bounds_line("goblin", &CINE_B_GOBLIN);
    bounds_line("droid", &CINE_B_DROID);
    bounds_line("alien", &CINE_B_ALIEN);

    if (fails) { printf("cinematic camera check FAILED (%d)\n", fails); return 1; }
    printf("cinematic camera check PASSED\n");
    return 0;
}
