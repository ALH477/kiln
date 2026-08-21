/* SPDX-License-Identifier: MPL-2.0
 *
 * pm-cine-check.c — host assertions over PetaByte Madness' camera validator.
 *
 * Driven by nix/checks/pm-cine.nix. Same shape and the same reasoning as
 * kiln-logic-check.c: pm_cine_lint.c and pm_camkey.h are pure arithmetic over
 * caller-owned structs, so they compile natively against nix/checks/stub/ and
 * can be asserted on in seconds.
 *
 * ── What is actually being verified here ───────────────────────────────
 * NOT the game's real camera tables — those cannot leave the ROM build
 * (pm_demo.c needs libdragon and Tiny3D, and FLYOVER_KEYS is filled at runtime
 * by flyover_build_keys), and the `PM_CINE_LINT=1` ROM inspects them where they
 * live.
 *
 * What this verifies is THE DETECTOR, in both directions, which is the half a
 * console report cannot check itself. Every hard-failure flag gets a table that
 * trips exactly it and nothing else, and the clean table has to come back with
 * zero. A gate that has only ever been seen to pass is a gate that might not be
 * checking anything — this repo has that rule written down, and it is written
 * down because two of its checks turned out to be doing nothing.
 *
 * The measurements (overshoot, hitch) are checked as NUMBERS rather than as
 * flags, because a threshold is a matter of taste and an arithmetic result is
 * not. If PM_CINE_OVERSHOOT_FRAC is ever retuned, the assertion below should
 * still hold.
 */
#include <stdarg.h>
#include <stdio.h>
#include <math.h>

#include "pm_cine_lint.h"

static int g_fail;

static void ok(int cond, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs(cond ? "  ok   " : "  FAIL ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
    if (!cond) g_fail++;
}

/* ────────────────────────────────────────────────────────────────────────
 * The clean table.
 *
 * Four keys, evenly spaced in time, the eye tracking a straight line while the
 * look target holds — the shape of a simple dolly. Round numbers so every
 * expected value below can be stated exactly rather than approximated.
 *
 * Evenly spaced is load-bearing for one of them: a Catmull-Rom tangent comes
 * from a key's two neighbours, so uniform spacing along a straight line gives a
 * curve that IS the straight line, and the overshoot measurement must therefore
 * report zero. That makes the non-zero cases below mean something.
 */
static const PMCamKey CLEAN[] = {
    { 0.0f, {{   0.0f, 100.0f,   0.0f }}, {{ 0.0f, 100.0f, 400.0f }} },
    { 1.0f, {{ 100.0f, 100.0f,   0.0f }}, {{ 0.0f, 100.0f, 400.0f }} },
    { 2.0f, {{ 200.0f, 100.0f,   0.0f }}, {{ 0.0f, 100.0f, 400.0f }} },
    { 3.0f, {{ 300.0f, 100.0f,   0.0f }}, {{ 0.0f, 100.0f, 400.0f }} },
};

static PMCineShot clean_shot(void)
{
    PMCineShot s;
    s.keys = CLEAN;
    s.key_count = (int)(sizeof CLEAN / sizeof CLEAN[0]);
    s.duration = 3.0f;
    s.near_z = 10.0f;
    s.far_z = 4000.0f;
    s.loop = 0;
    return s;
}

static void test_clean(void)
{
    PMCineReport r;
    PMCineShot s = clean_shot();
    const uint32_t err = pm_cine_lint(&s, NULL, &r);

    puts("pm_cine_lint: the clean table");
    ok(err == 0, "no hard failures (got 0x%04x)", err);
    ok(r.bad_key == -1, "no key implicated (got %d)", r.bad_key);

    /* A uniform straight line IS its own Catmull-Rom curve. Anything else here
     * means the sampler and the chord disagree about geometry, which would
     * make every overshoot number reported elsewhere meaningless. */
    ok(r.overshoot < 1e-4f, "straight uniform line has no bulge (got %.6f)",
       (double)r.overshoot);

    /* Equal chords over equal spans. */
    ok(fabsf(r.speed_ratio - 1.0f) < 1e-4f,
       "uniform spacing gives speed ratio 1 (got %.4f)", (double)r.speed_ratio);
    ok((r.note & PM_CINE_NOTE_HITCH) == 0, "no hitch noted");
    ok((r.note & PM_CINE_NOTE_OVERSHOOT) == 0, "no overshoot noted");
    ok((r.note & PM_CINE_NOTE_LATE_START) == 0, "starts at t = 0");
    ok(fabsf(r.tail) < 1e-4f, "no dead tail (got %.4f)", (double)r.tail);
    /* The WORST aim distance over the table, not the first: the eye tracks out
     * to x = 300 while the look target holds at z = 400, so the last key is
     * sqrt(300^2 + 400^2) = 500 away from what it is pointed at. That maximum
     * is the number SUBJECT_CUT has to be compared against — checking the
     * first key instead would pass a shot whose end is behind the far plane. */
    ok(fabsf(r.subject_dist - 500.0f) < 1.0f,
       "worst subject distance measured (got %.1f, want 500)",
       (double)r.subject_dist);
}

/* ────────────────────────────────────────────────────────────────────────
 * One table per hard failure. Each is the clean table with ONE thing wrong,
 * and each asserts that the one thing is what fires — a detector that returned
 * "everything is broken" for every input would pass a test that only checked
 * the bit it was looking for.
 */
static void expect_only(const char *what, PMCineShot *s, uint32_t want)
{
    PMCineReport r;
    const uint32_t err = pm_cine_lint(s, NULL, &r);
    ok((err & want) != 0, "%s: raises %s", what, pm_cine_err_name(want));
    ok((err & ~want) == 0, "%s: raises NOTHING ELSE (got 0x%04x)", what, err);
}

static void test_failures(void)
{
    puts("pm_cine_lint: one defect at a time");

    /* No keys at all. */
    {
        PMCineShot s = clean_shot();
        s.key_count = 0;
        expect_only("empty table", &s, PM_CINE_ERR_NO_KEYS);
        s = clean_shot();
        s.keys = NULL;
        expect_only("NULL table", &s, PM_CINE_ERR_NO_KEYS);
    }

    /* Times out of order. */
    {
        static PMCamKey k[4];
        PMCineShot s = clean_shot();
        for (int i = 0; i < 4; i++) k[i] = CLEAN[i];
        k[2].t = 0.5f;                       /* behind its predecessor */
        s.keys = k;
        expect_only("time going backwards", &s, PM_CINE_ERR_TIME_ORDER);

        PMCineReport r;
        pm_cine_lint(&s, NULL, &r);
        ok(r.bad_key == 2, "implicates key 2 (got %d)", r.bad_key);

        /* Duplicated times bound a zero-span segment the sampler can never
         * traverse, so they are the same defect and must be caught too. */
        for (int i = 0; i < 4; i++) k[i] = CLEAN[i];
        k[2].t = k[1].t;
        expect_only("duplicated key time", &s, PM_CINE_ERR_TIME_ORDER);
    }

    /* A key past the shot's duration is unreachable. */
    {
        PMCineShot s = clean_shot();
        s.duration = 2.0f;                   /* last key sits at t = 3 */
        expect_only("key past duration", &s, PM_CINE_ERR_KEY_PAST_END);
    }

    /* eye == look: the zero-length view vector. */
    {
        static PMCamKey k[4];
        PMCineShot s = clean_shot();
        for (int i = 0; i < 4; i++) k[i] = CLEAN[i];
        k[1].look = k[1].eye;
        s.keys = k;
        expect_only("eye equal to look", &s, PM_CINE_ERR_DEGENERATE);
    }

    /* An inverted or negative frustum. */
    {
        PMCineShot s = clean_shot();
        s.far_z = 5.0f;                      /* under near_z = 10 */
        expect_only("far plane inside near", &s, PM_CINE_ERR_FRUSTUM);

        s = clean_shot();
        s.near_z = -1.0f;
        s.far_z = 4000.0f;
        expect_only("negative near plane", &s, PM_CINE_ERR_FRUSTUM);
    }

    /* The aim point beyond the far plane — "six shots lost their geometry". */
    {
        PMCineShot s = clean_shot();
        s.far_z = 200.0f;                    /* the look target is 400 out */
        expect_only("subject behind the far plane", &s,
                    PM_CINE_ERR_SUBJECT_CUT);
    }

    /* NaN anywhere. */
    {
        static PMCamKey k[4];
        PMCineShot s = clean_shot();
        for (int i = 0; i < 4; i++) k[i] = CLEAN[i];
        k[2].eye.v[1] = NAN;
        s.keys = k;
        expect_only("NaN in an eye position", &s, PM_CINE_ERR_NAN);

        PMCineReport r;
        pm_cine_lint(&s, NULL, &r);
        ok(r.bad_key == 2, "NaN implicates key 2 (got %d)", r.bad_key);
    }
}

/* ────────────────────────────────────────────────────────────────────────
 * The measurements. Checked as numbers, not as flags — a threshold is taste
 * and can be retuned; the arithmetic underneath it should not move.
 */
static void test_overshoot(void)
{
    puts("pm_cine_lint: measurements");

    /* Uneven spacing next to a hold — the exact shape pm_intake.c's three
     * hand-inserted midpoint keys exist to suppress. The tangent at key 1
     * comes from keys 0 and 2, and key 0 is a long way away, so the curve
     * leaving key 1 overshoots the short segment that follows. */
    static const PMCamKey SWING[] = {
        { 0.0f, {{    0.0f, 100.0f, 0.0f }}, {{ 0.0f, 0.0f, 400.0f }} },
        { 1.0f, {{ 1000.0f, 100.0f, 0.0f }}, {{ 0.0f, 0.0f, 400.0f }} },
        { 2.0f, {{ 1020.0f, 100.0f, 0.0f }}, {{ 0.0f, 0.0f, 400.0f }} },
        { 3.0f, {{ 1040.0f, 100.0f, 0.0f }}, {{ 0.0f, 0.0f, 400.0f }} },
    };
    PMCineShot s;
    s.keys = SWING;
    s.key_count = 4;
    s.duration = 3.0f;
    s.near_z = 10.0f;
    s.far_z = 4000.0f;
    s.loop = 0;

    PMCineReport r;
    const uint32_t err = pm_cine_lint(&s, NULL, &r);
    ok(err == 0, "an ugly shot is not a BROKEN shot (got 0x%04x)", err);
    ok(r.overshoot > PM_CINE_OVERSHOOT_FRAC,
       "the swing is measured (%.3f of chord, threshold %.2f)",
       (double)r.overshoot, (double)PM_CINE_OVERSHOOT_FRAC);
    ok((r.note & PM_CINE_NOTE_OVERSHOOT) != 0, "and noted");
    ok(r.overshoot_seg == 1,
       "on segment 1, the short one after the long jump (got %d)",
       r.overshoot_seg);

    /* The hitch metric: same table, so the 1000-unit segment against the
     * 20-unit ones is a 50x ratio. */
    ok(fabsf(r.speed_ratio - 50.0f) < 0.5f,
       "speed ratio 1000/20 = 50 (got %.2f)", (double)r.speed_ratio);
    ok((r.note & PM_CINE_NOTE_HITCH) != 0, "hitch noted");
}

static void test_holds_are_not_hitches(void)
{
    /* A HOLD must not read as an infinitely slow segment and drag the ratio to
     * infinity. The intake holds twice — on the sit/type beat and on the empty
     * bed — so a validator that flagged every held shot would be flagging the
     * game's best-composed cutscene for composing it. */
    static const PMCamKey HOLD[] = {
        { 0.0f, {{   0.0f, 100.0f, 0.0f }}, {{ 0.0f, 0.0f, 400.0f }} },
        { 1.0f, {{ 100.0f, 100.0f, 0.0f }}, {{ 0.0f, 0.0f, 400.0f }} },
        { 2.0f, {{ 100.0f, 100.0f, 0.0f }}, {{ 0.0f, 0.0f, 400.0f }} },  /* hold */
        { 3.0f, {{ 200.0f, 100.0f, 0.0f }}, {{ 0.0f, 0.0f, 400.0f }} },
    };
    PMCineShot s;
    s.keys = HOLD;
    s.key_count = 4;
    s.duration = 3.0f;
    s.near_z = 10.0f;
    s.far_z = 4000.0f;
    s.loop = 0;

    PMCineReport r;
    const uint32_t err = pm_cine_lint(&s, NULL, &r);
    puts("pm_cine_lint: a hold is a composition, not a defect");
    ok(err == 0, "a held shot is clean (got 0x%04x)", err);
    ok((r.note & PM_CINE_NOTE_HITCH) == 0, "and is not called a hitch");
    ok(fabsf(r.speed_ratio - 1.0f) < 1e-3f,
       "the two moving segments match (got %.4f)", (double)r.speed_ratio);
}

static void test_bounds_and_tail(void)
{
    puts("pm_cine_lint: containment and dead tail");

    PMCineBounds b;
    b.mins = (fm_vec3_t){{ -50.0f, 0.0f, -50.0f }};
    b.maxs = (fm_vec3_t){{ 150.0f, 200.0f, 150.0f }};
    b.valid = 1;

    PMCineShot s = clean_shot();
    PMCineReport r;
    const uint32_t err = pm_cine_lint(&s, &b, &r);

    /* CLEAN runs the eye out to x = 300, well past the box. */
    ok(err == 0, "outside the room is not a hard failure (got 0x%04x)", err);
    ok((r.note & PM_CINE_NOTE_OUTSIDE) != 0, "but it is noted");
    ok(r.outside_key == 2, "at the first key that leaves (got %d)",
       r.outside_key);

    b.valid = 0;
    pm_cine_lint(&s, &b, &r);
    ok((r.note & PM_CINE_NOTE_OUTSIDE) == 0,
       "and the check is skippable for exteriors");

    /* Dead tail: the camera stops keyframing well before the shot ends. */
    s = clean_shot();
    s.duration = 10.0f;      /* last key at t = 3 */
    pm_cine_lint(&s, NULL, &r);
    ok(fabsf(r.tail - 7.0f) < 1e-3f, "tail measured (got %.3f, want 7)",
       (double)r.tail);
    ok((r.note & PM_CINE_NOTE_DEAD_TAIL) != 0, "and noted");
    ok((r.err & PM_CINE_ERR_KEY_PAST_END) == 0,
       "a long tail is not a key past the end");
}

static void test_sampler_hits_its_keys(void)
{
    /* The property the whole module rests on: Catmull-Rom passes exactly
     * THROUGH every control point. If it did not, the overshoot measured
     * against the key chords would be measuring the sampler's own error. */
    puts("pm_camkey_sample: passes through every key");
    const int n = (int)(sizeof CLEAN / sizeof CLEAN[0]);
    int worst = 0;
    float worst_d = 0.0f;
    for (int i = 0; i < n; i++) {
        fm_vec3_t eye;
        pm_camkey_sample(CLEAN, n, 0, CLEAN[i].t, &eye, NULL);
        float d = 0.0f;
        for (int a = 0; a < 3; a++) {
            const float e = eye.v[a] - CLEAN[i].eye.v[a];
            d += e * e;
        }
        d = sqrtf(d);
        if (d > worst_d) { worst_d = d; worst = i; }
    }
    ok(worst_d < 1e-3f, "worst key error %.6f (key %d)", (double)worst_d,
       worst);

    /* And it must be defined outside its own range rather than reading off
     * the end of the table — pm_demo_update clamps at `duration`, but
     * pm_debug.c samples the curve for drawing and pm_cine seeks. */
    fm_vec3_t a, c;
    pm_camkey_sample(CLEAN, n, 0, -5.0f, &a, NULL);
    pm_camkey_sample(CLEAN, n, 0, 999.0f, &c, NULL);
    ok(fabsf(a.v[0] - CLEAN[0].eye.v[0]) < 1e-3f,
       "clamps to the first key before t=0 (got %.3f)", (double)a.v[0]);
    ok(fabsf(c.v[0] - CLEAN[n - 1].eye.v[0]) < 1e-3f,
       "clamps to the last key past the end (got %.3f)", (double)c.v[0]);
}

int main(void)
{
    test_clean();
    test_failures();
    test_overshoot();
    test_holds_are_not_hitches();
    test_bounds_and_tail();
    test_sampler_hits_its_keys();

    if (g_fail) {
        printf("\n%d assertion(s) FAILED\n", g_fail);
        return 1;
    }
    puts("\nall pm_cine assertions passed");
    return 0;
}
