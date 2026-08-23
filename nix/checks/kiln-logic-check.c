/* SPDX-License-Identifier: MIT
 *
 * kiln-logic-check.c — host assertions over the engine's pure-logic modules.
 *
 * Driven by nix/checks/kiln-logic.nix; see that file for why these modules can
 * be compiled natively at all, and for the stub surface they run against.
 *
 * ── What is being bought ───────────────────────────────────────────────
 * Before this existed, exactly one engine module (kiln_asset) had any test, and
 * everything else was verified by building a ROM and looking at it. That is a
 * bad deal for pure arithmetic: kiln_clip's slab traces and SlideMove iteration
 * are the code PetaByte Madness' whole first-person section rests on, and their
 * failure modes on console are "the player is stuck on nothing" and "the player
 * walked through a wall" — both of which look like level-design mistakes and
 * neither of which a screenshot distinguishes from one.
 *
 * The bias throughout is towards the properties that are cheap to state and
 * expensive to discover on hardware: a fraction that must be exactly 1, a
 * normal that must point back along the axis of approach, two code paths that
 * must agree, and a handle that must be refused after its slot is reused.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "kiln_clip.h"
#include "kiln_dict.h"
#include "kiln_cache.h"
#include "kiln_lod.h"
#include "kiln_rng.h"
#include "kiln_stream.h"
#include "kiln_voxel.h"

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

static fm_vec3_t V(float x, float y, float z)
{
    fm_vec3_t v = {{ x, y, z }};
    return v;
}

#define NEAR(a, b, eps) (fabsf((a) - (b)) <= (eps))

/* ────────────────────────────────────────────────────────────────────────
 * kiln_clip
 *
 * One room: a floor slab and four walls, sized so a 16-unit player box has
 * real space to move in. Numbers are round so an expected fraction can be
 * stated exactly rather than approximated.
 * ──────────────────────────────────────────────────────────────────────── */
#define FLOOR_TOP    0.0f
#define WALL_X       100.0f
#define WALL_Z       100.0f
#define SURF_FLOOR   1
#define SURF_WALL    2

static const KilnBrush ROOM[] = {
    /* floor: top face at y=0                                             */
    { {{ -WALL_X, -32.0f, -WALL_Z }}, {{ WALL_X, FLOOR_TOP, WALL_Z }}, SURF_FLOOR, 0, {0, 0} },
    /* +X wall                                                            */
    { {{  WALL_X, -32.0f, -WALL_Z }}, {{ WALL_X + 32.0f, 128.0f, WALL_Z }}, SURF_WALL, 0, {0, 0} },
    /* -X wall                                                            */
    { {{ -WALL_X - 32.0f, -32.0f, -WALL_Z }}, {{ -WALL_X, 128.0f, WALL_Z }}, SURF_WALL, 0, {0, 0} },
    /* +Z wall                                                            */
    { {{ -WALL_X, -32.0f,  WALL_Z }}, {{ WALL_X, 128.0f, WALL_Z + 32.0f }}, SURF_WALL, 0, {0, 0} },
    /* -Z wall                                                            */
    { {{ -WALL_X, -32.0f, -WALL_Z - 32.0f }}, {{ WALL_X, 128.0f, -WALL_Z }}, SURF_WALL, 0, {0, 0} },
};
#define ROOM_N ((uint16_t)(sizeof ROOM / sizeof ROOM[0]))

static const fm_vec3_t BOX_MINS = {{ -8.0f, -16.0f, -8.0f }};
static const fm_vec3_t BOX_MAXS = {{  8.0f,  16.0f,  8.0f }};

static void test_clip(void)
{
    puts("── kiln_clip ──");

    /* An empty world must never block. A trace that reports a hit against no
     * brushes freezes a player in an unloaded room, which reads as a hang. */
    kiln_clip_set_world(NULL, 0);
    KilnTrace t = kiln_clip_box(V(0, 20, 0), V(500, 20, 500), BOX_MINS, BOX_MAXS);
    ok(t.fraction == 1.0f, "empty world: fraction is exactly 1 (%.6f)", t.fraction);

    kiln_clip_set_world(ROOM, ROOM_N);
    kiln_clip_set_broadphase(0);

    /* Clean move through open space. Exactly 1, not 0.999: kiln_player
     * integrates against this every frame and a fraction that is merely close
     * to 1 bleeds position away on every step. */
    t = kiln_clip_box(V(0, 20, 0), V(50, 20, 0), BOX_MINS, BOX_MAXS);
    ok(t.fraction == 1.0f, "open move: fraction is exactly 1 (%.6f)", t.fraction);
    ok(NEAR(t.endpos.v[0], 50.0f, 1e-4f),
       "open move: endpos is the requested end (%.3f)", t.endpos.v[0]);

    /* Into the +X wall. The box's +X extent is 8, so its centre stops at
     * WALL_X - 8 = 92, give or take the module's 1e-3 contact epsilon. */
    t = kiln_clip_box(V(0, 20, 0), V(200, 20, 0), BOX_MINS, BOX_MAXS);
    ok(t.fraction < 1.0f, "into +X wall: blocked (fraction %.4f)", t.fraction);
    ok(t.endpos.v[0] <= WALL_X - 8.0f + 1e-2f,
       "into +X wall: box stops outside it (x=%.3f, wall at %.0f)",
       t.endpos.v[0], WALL_X);
    ok(NEAR(t.normal.v[0], -1.0f, 1e-3f) &&
       NEAR(t.normal.v[1], 0.0f, 1e-3f) && NEAR(t.normal.v[2], 0.0f, 1e-3f),
       "into +X wall: normal points back at the mover (%.2f %.2f %.2f)",
       t.normal.v[0], t.normal.v[1], t.normal.v[2]);
    ok(t.hitsurface == SURF_WALL,
       "into +X wall: hitsurface is the wall's (%u)", t.hitsurface);

    /* Downward onto the floor, and the surface id must come from the FLOOR
     * brush — kiln_player keys its footstep SFX off exactly this, so a trace
     * that reported the wrong brush's surface would play stone on metal. */
    t = kiln_clip_box(V(0, 40, 0), V(0, 0, 0), BOX_MINS, BOX_MAXS);
    ok(t.fraction < 1.0f, "down onto floor: blocked (fraction %.4f)", t.fraction);
    ok(NEAR(t.normal.v[1], 1.0f, 1e-3f),
       "down onto floor: normal is up (%.2f)", t.normal.v[1]);
    ok(t.hitsurface == SURF_FLOOR,
       "down onto floor: hitsurface is the floor's (%u)", t.hitsurface);

    /* kiln_clip_ground: standing ON the floor must find it; well above it
     * must not. The probe is documented as 2 units, so 40 units up is
     * comfortably outside it. */
    t = kiln_clip_ground(V(0, 16.0f, 0), BOX_MINS, BOX_MAXS);
    ok(t.fraction < 1.0f, "ground: found while standing on the floor");
    t = kiln_clip_ground(V(0, 60.0f, 0), BOX_MINS, BOX_MAXS);
    ok(t.fraction == 1.0f, "ground: not found 44 units above the floor");

    /* ── SlideMove ──────────────────────────────────────────────────────
     * The property that matters is not "it moved" but "it kept the
     * component of the move the wall did not block". A single trace would
     * stop the player dead on any diagonal approach; that is the difference
     * between OoT-feeling movement and getting caught on every corner. */
    fm_vec3_t p = kiln_clip_slide(V(0, 20, 0), V(200, 0, 40),
                                 BOX_MINS, BOX_MAXS, 4);
    ok(p.v[0] <= WALL_X - 8.0f + 1e-2f,
       "slide: does not end up inside the wall (x=%.3f)", p.v[0]);
    ok(p.v[2] > 20.0f,
       "slide: KEPT the unblocked Z component (z=%.3f of a requested 40)",
       p.v[2]);

    /* Into a corner, where both axes are blocked. The requirement is that it
     * terminates outside both walls — the iteration cap must not leave the
     * mover embedded in geometry after giving up. */
    p = kiln_clip_slide(V(80, 20, 80), V(200, 0, 200), BOX_MINS, BOX_MAXS, 4);
    ok(p.v[0] <= WALL_X - 8.0f + 1e-2f && p.v[2] <= WALL_Z - 8.0f + 1e-2f,
       "slide into a corner: ends outside both walls (%.3f, %.3f)",
       p.v[0], p.v[2]);

    /* A blocked slide must not move the mover BACKWARDS. A sign error in the
     * clip step produces exactly that, and on console it reads as the player
     * being shoved away from walls. */
    p = kiln_clip_slide(V(0, 20, 0), V(200, 0, 0), BOX_MINS, BOX_MAXS, 4);
    ok(p.v[0] >= 0.0f, "slide: a blocked move never goes backwards (%.3f)", p.v[0]);

    /* ── Broadphase equivalence ─────────────────────────────────────────
     * The grid is an optimisation, so its only correctness requirement is
     * that it changes nothing. This is the check that makes turning it on
     * safe: same world, same traces, same answers, fewer brushes examined.
     * kiln_clip.h says kiln_clip_ray always uses the flat walk, so the brush
     * count is compared for the box path only. */
    struct { fm_vec3_t a, b; } moves[] = {
        { V(0, 20, 0),    V(200, 20, 0) },
        { V(0, 20, 0),    V(-200, 20, 0) },
        { V(0, 20, 0),    V(0, 20, 200) },
        { V(0, 40, 0),    V(0, 0, 0) },
        { V(-90, 20, -90), V(90, 20, 90) },
        { V(50, 20, 50),  V(50, 20, 50) },   /* zero-length move */
    };
    const int nmoves = (int)(sizeof moves / sizeof moves[0]);
    int agree = 1;
    uint16_t flat_total = 0, grid_total = 0;
    for (int i = 0; i < nmoves; i++) {
        kiln_clip_set_broadphase(0);
        kiln_clip_set_world(ROOM, ROOM_N);
        KilnTrace a = kiln_clip_box(moves[i].a, moves[i].b, BOX_MINS, BOX_MAXS);
        flat_total += kiln_clip_last_trace_brushes();

        kiln_clip_set_broadphase(1);
        kiln_clip_set_world(ROOM, ROOM_N);
        KilnTrace b = kiln_clip_box(moves[i].a, moves[i].b, BOX_MINS, BOX_MAXS);
        grid_total += kiln_clip_last_trace_brushes();

        if (!NEAR(a.fraction, b.fraction, 1e-4f) ||
            a.hitsurface != b.hitsurface ||
            !NEAR(a.endpos.v[0], b.endpos.v[0], 1e-3f) ||
            !NEAR(a.endpos.v[1], b.endpos.v[1], 1e-3f) ||
            !NEAR(a.endpos.v[2], b.endpos.v[2], 1e-3f)) {
            printf("       move %d disagrees: flat f=%.5f surf=%u  "
                   "grid f=%.5f surf=%u\n",
                   i, a.fraction, a.hitsurface, b.fraction, b.hitsurface);
            agree = 0;
        }
    }
    ok(agree, "broadphase on == broadphase off over %d traces", nmoves);
    ok(grid_total <= flat_total,
       "broadphase examines no more brushes than the flat walk (%u vs %u)",
       grid_total, flat_total);

    kiln_clip_set_broadphase(0);
    kiln_clip_set_world(ROOM, ROOM_N);

    /* A ray is the box trace with zero extents, so it reaches 8 units
     * further before contact than the box does. Worth stating because the
     * camera boom depends on it. */
    KilnTrace r = kiln_clip_ray(V(0, 20, 0), V(200, 20, 0));
    ok(r.fraction < 1.0f, "ray: blocked by the wall (fraction %.4f)", r.fraction);
    ok(r.endpos.v[0] > t.endpos.v[0] || r.endpos.v[0] >= WALL_X - 1e-2f,
       "ray: reaches the wall face itself, not a box's standoff (x=%.3f)",
       r.endpos.v[0]);
}

/* ────────────────────────────────────────────────────────────────────────
 * kiln_dict
 * ──────────────────────────────────────────────────────────────────────── */
static void test_dict(void)
{
    puts("── kiln_dict ──");

    KilnDict d;
    kiln_dict_init(&d);
    ok(!kiln_dict_has_int(&d, "hp"), "a fresh dict has nothing in it");
    ok(kiln_dict_get_int(&d, "hp", -7) == -7,
       "a missing key returns the caller's default");

    kiln_dict_set_int(&d, "hp", 42);
    kiln_dict_set_float(&d, "speed", 2.5f);
    kiln_dict_set_vec3(&d, "origin", V(1, 2, 3));
    kiln_dict_set_str(&d, "name", "imp");

    ok(kiln_dict_get_int(&d, "hp", 0) == 42, "int round-trips");
    ok(NEAR(kiln_dict_get_float(&d, "speed", 0.0f), 2.5f, 1e-6f),
       "float round-trips");
    fm_vec3_t got = kiln_dict_get_vec3(&d, "origin", V(0, 0, 0));
    ok(got.v[0] == 1.0f && got.v[1] == 2.0f && got.v[2] == 3.0f,
       "vec3 round-trips (%.0f %.0f %.0f)", got.v[0], got.v[1], got.v[2]);
    ok(strcmp(kiln_dict_get_str(&d, "name", ""), "imp") == 0,
       "string round-trips");

    /* Overwriting the same key must not append. The dict is 16 slots and
     * spawn args are authored content — a set that appended would silently
     * exhaust the dict on any key written twice. */
    kiln_dict_set_int(&d, "hp", 99);
    ok(kiln_dict_get_int(&d, "hp", 0) == 99, "overwrite replaces the value");
    ok(d.count == 4, "overwrite does not append a second slot (count %u)",
       d.count);

    /* The typed getters must refuse a type mismatch rather than reinterpret
     * the union. Reading an int out of a float slot would return whatever the
     * float's bit pattern happens to be, which is a plausible-looking number. */
    ok(kiln_dict_get_int(&d, "speed", -1) == -1,
       "reading an int from a float key returns the default");
    ok(!kiln_dict_has_vec3(&d, "hp"), "has_vec3 is false for an int key");
    ok(kiln_dict_has_int(&d, "hp") && kiln_dict_has_float(&d, "speed") &&
       kiln_dict_has_vec3(&d, "origin") && kiln_dict_has_str(&d, "name"),
       "each typed check is true for its own key");

    /* Key interning: two dicts must agree about a key without either having
     * seen the other, since the key table is module-global. */
    KilnDict e;
    kiln_dict_init(&e);
    kiln_dict_set_int(&e, "hp", 5);
    ok(kiln_dict_get_int(&e, "hp", 0) == 5 && kiln_dict_get_int(&d, "hp", 0) == 99,
       "two dicts sharing an interned key keep separate values");

    /* set_auto is the mapping from .map text to typed args. This is where a
     * regression is silent: "0 0 0" landing as a STRING makes every
     * origin-derived spawn sit at the world origin, and the map still loads. */
    puts("── kiln_dict: set_auto (the .map text mapping) ──");
    KilnDict a;
    kiln_dict_init(&a);
    kiln_dict_set_auto(&a, "origin",  "10 20 30");
    kiln_dict_set_auto(&a, "health",  "100");
    kiln_dict_set_auto(&a, "scale",   "1.5");
    kiln_dict_set_auto(&a, "target",  "door_a");
    kiln_dict_set_auto(&a, "negative", "-5");
    kiln_dict_set_auto(&a, "negfloat", "-0.25");

    ok(kiln_dict_has_vec3(&a, "origin"), "\"10 20 30\" -> vec3");
    got = kiln_dict_get_vec3(&a, "origin", V(0, 0, 0));
    ok(got.v[0] == 10.0f && got.v[1] == 20.0f && got.v[2] == 30.0f,
       "the vec3's components are in order (%.0f %.0f %.0f)",
       got.v[0], got.v[1], got.v[2]);
    ok(kiln_dict_has_int(&a, "health"), "\"100\" -> int");
    ok(kiln_dict_has_float(&a, "scale"), "\"1.5\" -> float");
    ok(kiln_dict_has_str(&a, "target"), "\"door_a\" -> string");
    ok(kiln_dict_get_int(&a, "negative", 0) == -5, "\"-5\" -> int -5");
    ok(NEAR(kiln_dict_get_float(&a, "negfloat", 0.0f), -0.25f, 1e-6f),
       "\"-0.25\" -> float -0.25");

    ok(kiln_dict_parse_int("7") == 7, "parse_int");
    ok(NEAR(kiln_dict_parse_float("7.5"), 7.5f, 1e-6f), "parse_float");
    got = kiln_dict_parse_vec3("-1 0.5 2");
    ok(NEAR(got.v[0], -1.0f, 1e-6f) && NEAR(got.v[1], 0.5f, 1e-6f) &&
       NEAR(got.v[2], 2.0f, 1e-6f),
       "parse_vec3 handles mixed signs and decimals (%.2f %.2f %.2f)",
       got.v[0], got.v[1], got.v[2]);
}

/* ────────────────────────────────────────────────────────────────────────
 * kiln_cache
 * ──────────────────────────────────────────────────────────────────────── */
static int g_loads, g_releases;

static void *fake_load(const char *key, void *ctx)
{
    (void)ctx;
    g_loads++;
    char *s = malloc(strlen(key) + 1);
    strcpy(s, key);
    return s;
}

static void fake_release(void *res, void *ctx)
{
    (void)ctx;
    g_releases++;
    free(res);
}

/* A loader that cannot find its asset — the case a ROM hits when a model is
 * absent from the filesystem image. */
static void *fail_load(const char *key, void *ctx)
{
    (void)key; (void)ctx;
    return NULL;
}

static void test_cache(void)
{
    puts("── kiln_cache ──");

    KilnCache c;
    kiln_cache_init(&c);
    ok(kiln_cache_count(&c) == 0, "a fresh cache is empty");

    KilnCacheHandle h1 = kiln_cache_acquire(&c, "tiles/0_0/lod0.geom",
                                          fake_load, fake_release, NULL);
    ok(h1 != KILN_CACHE_HANDLE_INVALID, "acquire returns a handle");
    ok(g_loads == 1, "acquire loaded once (%d)", g_loads);
    ok(strcmp((char *)kiln_cache_resolve(&c, h1), "tiles/0_0/lod0.geom") == 0,
       "resolve returns the loaded resource");

    /* The whole point of the module: a second acquire of the same key must
     * NOT load again. openworld streaming calls this per tile per frame. */
    KilnCacheHandle h2 = kiln_cache_acquire(&c, "tiles/0_0/lod0.geom",
                                          fake_load, fake_release, NULL);
    ok(g_loads == 1, "re-acquiring a cached key does not load again (%d)", g_loads);
    ok(h2 == h1, "the same key yields the same handle");
    ok(kiln_cache_refcount(&c, h1) == 2, "refcount is 2 (%u)",
       kiln_cache_refcount(&c, h1));
    ok(kiln_cache_count(&c) == 1, "still one entry (%u)", kiln_cache_count(&c));

    ok(kiln_cache_release(&c, h1, fake_release, NULL) == 0,
       "the first release does not free (references remain)");
    ok(g_releases == 0, "release_fn not called yet (%d)", g_releases);
    ok(kiln_cache_release(&c, h2, fake_release, NULL) == 1,
       "the last release frees");
    ok(g_releases == 1, "release_fn called exactly once (%d)", g_releases);
    ok(kiln_cache_count(&c) == 0, "the slot is free again");

    /* ── The reason handles carry a generation ───────────────────────────
     * A stale handle must resolve to NULL, not to whatever now occupies the
     * reused slot. Without this, a tile unloaded and a different tile loaded
     * into the same slot makes an old handle silently address the new tile's
     * geometry — which draws the wrong mesh at the right place, one of the
     * least diagnosable bugs available on this hardware. */
    KilnCacheHandle reused = kiln_cache_acquire(&c, "tiles/9_9/lod2.geom",
                                              fake_load, fake_release, NULL);
    ok(reused != KILN_CACHE_HANDLE_INVALID, "a new key takes the freed slot");
    ok(kiln_cache_resolve(&c, h1) == NULL,
       "the STALE handle resolves to NULL, not to the slot's new occupant");
    ok(reused != h1, "the new handle differs from the stale one (generation)");
    ok(kiln_cache_refcount(&c, h1) == 0, "a stale handle reports refcount 0");
    ok(kiln_cache_release(&c, h1, fake_release, NULL) == -1,
       "releasing a stale handle is refused, not applied to the new resource");
    ok(kiln_cache_refcount(&c, reused) == 1,
       "the live entry's refcount is untouched by the stale release (%u)",
       kiln_cache_refcount(&c, reused));

    ok(kiln_cache_resolve(&c, KILN_CACHE_HANDLE_INVALID) == NULL,
       "the invalid handle resolves to NULL");

    /* A load that FAILS must not occupy a slot. kiln_cache_acquire is
     * documented to return INVALID when load_fn returns NULL; if it kept the
     * slot anyway, a level whose assets are missing would fill the cache with
     * nothing and then refuse every asset that is present — a missing file
     * turning into a cache-exhaustion bug several tiles later. */
    const uint16_t before = kiln_cache_count(&c);
    KilnCacheHandle bad = kiln_cache_acquire(&c, "missing", fail_load,
                                           fake_release, NULL);
    ok(bad == KILN_CACHE_HANDLE_INVALID, "a failed load returns INVALID");
    ok(kiln_cache_count(&c) == before,
       "a failed load leaves no slot behind (%u -> %u)",
       before, kiln_cache_count(&c));

    kiln_cache_release(&c, reused, fake_release, NULL);
    ok(kiln_cache_count(&c) == 0, "cache drains to empty (%u)",
       kiln_cache_count(&c));
}

/* ────────────────────────────────────────────────────────────────────────
 * kiln_lod
 * ──────────────────────────────────────────────────────────────────────── */
static void test_lod(void)
{
    puts("── kiln_lod ──");

    KilnLODConfig cfg;
    const float tile = 256.0f;
    kiln_lod_init_defaults(&cfg, tile);
    ok(cfg.threshold_count > 0, "defaults declare thresholds (%u)",
       cfg.threshold_count);

    /* Thresholds must be strictly increasing, or kiln_lod_select's scan
     * returns a level that is not the nearest match and a distant tile can
     * come out at a HIGHER detail than a near one. */
    int increasing = 1;
    for (int i = 1; i < cfg.threshold_count; i++)
        if (!(cfg.thresholds_sq[i] > cfg.thresholds_sq[i - 1])) increasing = 0;
    ok(increasing, "default thresholds are strictly increasing");

    ok(kiln_lod_select(&cfg, 0.0f) == 0, "at the camera: LOD 0");

    /* Monotonic: detail may only ever get coarser with distance. Stated as a
     * sweep rather than at three points, because an off-by-one in the scan
     * shows up at one boundary and nowhere else. */
    int monotonic = 1;
    uint8_t prev = 0;
    for (float d = 0.0f; d < tile * 40.0f; d += tile * 0.25f) {
        uint8_t lod = kiln_lod_select(&cfg, d * d);
        if (lod < prev) monotonic = 0;
        prev = lod;
    }
    ok(monotonic, "LOD never gets FINER as distance grows");

    ok(kiln_lod_visible(&cfg, 0.0f), "the camera's own tile is visible");
    ok(!kiln_lod_visible(&cfg, cfg.thresholds_sq[cfg.threshold_count - 1] * 4.0f),
       "beyond the last threshold: not visible");
    ok(kiln_lod_visible(&cfg, cfg.thresholds_sq[cfg.threshold_count - 1]),
       "exactly at the last threshold: still visible (inclusive bound)");

    /* The selector callback must agree with the direct call — it is the form
     * kiln_tile_update actually uses, so a divergence means the shipping path
     * is the untested one. */
    int cb_agrees = 1;
    for (float d = 0.0f; d < tile * 20.0f; d += tile * 0.5f)
        if (kiln_lod_selector_cb(0, 0, d * d, &cfg) != kiln_lod_select(&cfg, d * d))
            cb_agrees = 0;
    ok(cb_agrees, "kiln_lod_selector_cb agrees with kiln_lod_select");

    /* An empty config must treat everything as visible at full detail rather
     * than culling the world — a zeroed struct is a plausible caller state. */
    KilnLODConfig zero;
    memset(&zero, 0, sizeof zero);
    ok(kiln_lod_visible(&zero, 1e9f),
       "a zeroed config draws everything rather than nothing");
}

/* ────────────────────────────────────────────────────────────────────────
 * kiln_rng
 * ──────────────────────────────────────────────────────────────────────── */
static void test_rng(void)
{
    puts("── kiln_rng ──");

    KilnRng a, b;
    kiln_rng_seed(&a, 12345);
    kiln_rng_seed(&b, 12345);
    int same = 1;
    for (int i = 0; i < 256; i++)
        if (kiln_rng_u32(&a) != kiln_rng_u32(&b)) same = 0;
    ok(same, "the same seed gives the same sequence (256 draws)");

    /* A zero seed is documented as remapped to 1 rather than left stuck.
     * xorshift with state 0 returns 0 forever, which as a dice roll is a
     * loaded die and as a scatter is every prop at the same place. */
    KilnRng z;
    kiln_rng_seed(&z, 0);
    uint32_t first = kiln_rng_u32(&z);
    int varies = 0;
    for (int i = 0; i < 16; i++) if (kiln_rng_u32(&z) != first) varies = 1;
    ok(varies, "a zero seed still produces a varying sequence");

    kiln_rng_seed(&a, 999);
    int in_unit = 1, in_range = 1;
    int hist[6] = { 0 };
    for (int i = 0; i < 20000; i++) {
        float f = kiln_rng_f32(&a);
        if (!(f >= 0.0f && f < 1.0f)) in_unit = 0;
        int r = kiln_rng_range(&a, 0, 6);
        if (r < 0 || r > 5) in_range = 0; else hist[r]++;
    }
    ok(in_unit, "kiln_rng_f32 stays in [0, 1)");
    ok(in_range, "kiln_rng_range(0, 6) stays in [0, 5]");

    /* Every face must actually come up. A modulo or shift mistake that made
     * one face unreachable would be invisible in play and fatal to a board
     * game — kiln_dice sits directly on this. */
    int all_faces = 1;
    for (int i = 0; i < 6; i++) if (hist[i] == 0) all_faces = 0;
    ok(all_faces, "every one of six faces occurs (%d %d %d %d %d %d)",
       hist[0], hist[1], hist[2], hist[3], hist[4], hist[5]);

    /* Roughly uniform: each face within 20% of 1/6 over 20k draws. Loose on
     * purpose — this is a "not badly biased" check, not a statistical test. */
    int balanced = 1;
    for (int i = 0; i < 6; i++)
        if (hist[i] < 20000 / 6 * 0.8 || hist[i] > 20000 / 6 * 1.2) balanced = 0;
    ok(balanced, "the six faces are within 20%% of uniform");

    /* A degenerate range must not read or write out of bounds or loop. */
    kiln_rng_seed(&a, 7);
    ok(kiln_rng_range(&a, 3, 4) == 3, "a one-wide range returns its only value");
}


/* ────────────────────────────────────────────────────────────────────────
 * kiln_stream
 *
 * The room/tile streaming pacer's admission policy. Its failure modes are
 * exactly kiln_tile's and kiln_room's own: a wrong priority compare loads
 * the WRONG tile first under pressure, which reads as "that asset popped in
 * late" — a frame-pacing bug indistinguishable from a content mistake in
 * any capture. The eviction rule is deliberately identical to
 * kiln_event_post's, so it is asserted the same way.
 * ──────────────────────────────────────────────────────────────────────── */
static void test_stream(void)
{
    puts("── kiln_stream ──");

    /* ── Priority ordering + admit-count budget ──────────────────────── */
    {
        KilnStreamBudget budget = { .max_admits_per_frame = 2, .max_bytes_per_frame = 1000 };
        KilnStream s;
        kiln_stream_init(&s, budget);

        KilnStreamHandle ha = kiln_stream_request(&s, "a", KILN_STREAM_NORMAL, 10.0f, 100, (void *)1);
        KilnStreamHandle hb = kiln_stream_request(&s, "b", KILN_STREAM_NORMAL, 5.0f, 100, (void *)2);
        KilnStreamHandle hc = kiln_stream_request(&s, "c", KILN_STREAM_NORMAL, 20.0f, 100, (void *)3);
        ok(ha && hb && hc, "three distinct requests each get a handle");
        ok(kiln_stream_pending_count(&s) == 3, "all three are outstanding (%u)",
           kiln_stream_pending_count(&s));

        kiln_stream_frame_begin(&s);
        ok(kiln_stream_admits_this_frame(&s) == 2,
           "admits stop at max_admits_per_frame (%u)", kiln_stream_admits_this_frame(&s));
        ok(kiln_stream_bytes_admitted_this_frame(&s) == 200,
           "byte total matches the two admitted (%u)", kiln_stream_bytes_admitted_this_frame(&s));

        int saw_a = 0, saw_b = 0, saw_c = 0;
        for (KilnStreamSlot *sl = kiln_stream_first_admitted(&s); sl;
             sl = kiln_stream_next_admitted(&s, sl)) {
            if (!strcmp(sl->key, "a")) saw_a = 1;
            if (!strcmp(sl->key, "b")) saw_b = 1;
            if (!strcmp(sl->key, "c")) saw_c = 1;
        }
        ok(saw_a && saw_b, "the two closer requests (rank 10, 5) were admitted");
        ok(!saw_c, "the farthest request (rank 20) was NOT admitted — deferred, not dropped");
        ok(kiln_stream_dropped_total(&s) == 0,
           "deferring under budget is not the same as dropping (%u)",
           kiln_stream_dropped_total(&s));
    }

    /* ── Byte budget stops admission even with admits to spare ───────── */
    {
        KilnStreamBudget budget = { .max_admits_per_frame = 10, .max_bytes_per_frame = 250 };
        KilnStream s;
        kiln_stream_init(&s, budget);
        kiln_stream_request(&s, "x", KILN_STREAM_NORMAL, 1.0f, 100, (void *)1);
        kiln_stream_request(&s, "y", KILN_STREAM_NORMAL, 2.0f, 100, (void *)2);
        kiln_stream_request(&s, "z", KILN_STREAM_NORMAL, 3.0f, 100, (void *)3);

        kiln_stream_frame_begin(&s);
        ok(kiln_stream_admits_this_frame(&s) == 2,
           "the byte budget (250) admits x+y (200) but not a third 100 (%u admitted)",
           kiln_stream_admits_this_frame(&s));
        ok(kiln_stream_bytes_admitted_this_frame(&s) == 200,
           "bytes admitted never exceeds the budget (%u)",
           kiln_stream_bytes_admitted_this_frame(&s));
    }

    /* ── A single oversized request must not starve forever ──────────── */
    {
        KilnStreamBudget budget = { .max_admits_per_frame = 10, .max_bytes_per_frame = 100 };
        KilnStream s;
        kiln_stream_init(&s, budget);
        kiln_stream_request(&s, "huge", KILN_STREAM_NORMAL, 0.0f, 5000, (void *)1);

        kiln_stream_frame_begin(&s);
        ok(kiln_stream_admits_this_frame(&s) == 1,
           "a request bigger than the whole per-frame budget is still forced "
           "through when nothing else is competing (%u admits)",
           kiln_stream_admits_this_frame(&s));
        ok(kiln_stream_bytes_admitted_this_frame(&s) == 5000,
           "the forced admission's real cost is reported, not clamped (%u)",
           kiln_stream_bytes_admitted_this_frame(&s));
    }

    /* ── Urgency beats rank, even a very close one ───────────────────── */
    {
        KilnStreamBudget budget = { .max_admits_per_frame = 1, .max_bytes_per_frame = 100000 };
        KilnStream s;
        kiln_stream_init(&s, budget);
        kiln_stream_request(&s, "close-but-normal", KILN_STREAM_NORMAL, 1.0f, 10, (void *)1);
        kiln_stream_request(&s, "far-but-urgent",   KILN_STREAM_URGENT, 100.0f, 10, (void *)2);

        kiln_stream_frame_begin(&s);
        KilnStreamSlot *only = kiln_stream_first_admitted(&s);
        ok(only && strcmp(only->key, "far-but-urgent") == 0,
           "URGENT wins over a merely-closer NORMAL request (got '%s')",
           only ? only->key : "(none)");
    }

    /* ── Idempotent re-request: refresh, not duplicate ───────────────── */
    {
        KilnStreamBudget budget = { .max_admits_per_frame = 1, .max_bytes_per_frame = 100000 };
        KilnStream s;
        kiln_stream_init(&s, budget);
        void *tag = (void *)0x1234;
        KilnStreamHandle h1 = kiln_stream_request(&s, "tile", KILN_STREAM_NORMAL, 50.0f, 10, tag);
        ok(kiln_stream_pending_count(&s) == 1, "one outstanding request");

        KilnStreamHandle h2 = kiln_stream_request(&s, "tile", KILN_STREAM_URGENT, 5.0f, 10, tag);
        ok(h1 == h2, "re-requesting the same key+tag returns the SAME handle");
        ok(kiln_stream_pending_count(&s) == 1,
           "and does not consume a second slot (%u)", kiln_stream_pending_count(&s));

        kiln_stream_request(&s, "other", KILN_STREAM_NORMAL, 1.0f, 10, (void *)0x5678);
        kiln_stream_frame_begin(&s);
        KilnStreamSlot *only = kiln_stream_first_admitted(&s);
        ok(only && strcmp(only->key, "tile") == 0,
           "the refreshed URGENT priority is what admission actually sees, "
           "not the stale NORMAL it was first requested at");
    }

    /* ── Pool-full eviction, the same rule as kiln_event_post ────────── */
    {
        KilnStream s;
        kiln_stream_init(&s, (KilnStreamBudget){ 0, 0 });

        char keybuf[KILN_STREAM_MAX_PENDING][16];
        int fill_ok = 1;
        for (int i = 0; i < KILN_STREAM_MAX_PENDING; i++) {
            snprintf(keybuf[i], sizeof keybuf[i], "fill%d", i);
            /* Larger i = larger rank = LESS urgent = more expendable. */
            KilnStreamHandle h = kiln_stream_request(&s, keybuf[i], KILN_STREAM_NORMAL,
                                                    (float)i, 1, (void *)(intptr_t)(i + 1));
            if (h == KILN_STREAM_HANDLE_INVALID) fill_ok = 0;
        }
        ok(fill_ok, "the pool is not full while filling it for the first time");
        ok(kiln_stream_pending_count(&s) == KILN_STREAM_MAX_PENDING,
           "the pool is exactly full (%u)", kiln_stream_pending_count(&s));
        ok(kiln_stream_high_water(&s) == KILN_STREAM_MAX_PENDING,
           "high_water tracks the fullest the pool has ever been (%u)",
           kiln_stream_high_water(&s));

        /* A HIGHER-priority newcomer (smaller rank) must evict the single
         * worst occupant (fill(MAX-1), the largest rank) and succeed. */
        KilnStreamHandle evictor = kiln_stream_request(&s, "evictor", KILN_STREAM_NORMAL,
                                                       -1.0f, 1, (void *)999);
        ok(evictor != KILN_STREAM_HANDLE_INVALID,
           "a higher-priority request evicts the worst PENDING occupant");
        ok(kiln_stream_dropped_total(&s) == 0,
           "an eviction is not counted as a drop (%u)", kiln_stream_dropped_total(&s));

        /* A LOWER-priority newcomer than everything now resident must be
         * refused outright, and reported as a genuine drop. */
        uint32_t dropped_before = kiln_stream_dropped_total(&s);
        KilnStreamHandle refused = kiln_stream_request(&s, "too-low-priority", KILN_STREAM_NORMAL,
                                                       99999.0f, 1, (void *)888);
        ok(refused == KILN_STREAM_HANDLE_INVALID,
           "a lower-priority request than everything resident is refused");
        ok(kiln_stream_dropped_total(&s) == dropped_before + 1,
           "and IS counted as a drop, unlike the successful eviction above (%u -> %u)",
           dropped_before, kiln_stream_dropped_total(&s));
    }

    /* ── ADMITTED slots are never eviction victims ───────────────────── */
    {
        KilnStreamBudget budget = { .max_admits_per_frame = 1, .max_bytes_per_frame = 100000 };
        KilnStream s;
        kiln_stream_init(&s, budget);

        /* "bait" is deliberately the WORST-priority item this pool will
         * ever hold (PREFETCH, the lowest urgency tier, an enormous rank) —
         * exactly what a search that (incorrectly) considered ADMITTED
         * slots as eviction candidates would pick as "globally worst". */
        KilnStreamHandle h_bait = kiln_stream_request(&s, "bait", KILN_STREAM_PREFETCH,
                                                      1e9f, 1, (void *)1);
        kiln_stream_frame_begin(&s);  /* only request so far: admitted regardless of priority */
        ok(kiln_stream_admits_this_frame(&s) == 1, "bait is admitted (nothing else competing yet)");

        for (int i = 0; i < KILN_STREAM_MAX_PENDING - 1; i++) {
            char key[16];
            snprintf(key, sizeof key, "fill%d", i);
            kiln_stream_request(&s, key, KILN_STREAM_NORMAL, (float)i, 1, (void *)(intptr_t)(i + 2));
        }
        ok(kiln_stream_pending_count(&s) == KILN_STREAM_MAX_PENDING, "pool is exactly full");

        /* Better than the worst PENDING fill, but this must NOT be
         * satisfied by evicting "bait" even though bait looks like the
         * worse target by priority alone. */
        KilnStreamHandle newcomer = kiln_stream_request(&s, "newcomer", KILN_STREAM_NORMAL,
                                                        -1.0f, 1, (void *)9999);
        ok(newcomer != KILN_STREAM_HANDLE_INVALID,
           "a request better than the worst PENDING fill still finds room");
        ok(kiln_stream_cancel(&s, h_bait) == 0,
           "the ADMITTED 'bait' slot survived — eviction only ever considers PENDING slots");
    }

    /* ── cancel(): distinct outcomes for live vs. stale handles ──────── */
    {
        KilnStreamBudget budget = { .max_admits_per_frame = 5, .max_bytes_per_frame = 100000 };
        KilnStream s;
        kiln_stream_init(&s, budget);

        KilnStreamHandle h = kiln_stream_request(&s, "cancel-me", KILN_STREAM_NORMAL, 1.0f, 1, NULL);
        ok(kiln_stream_cancel(&s, h) == 0, "cancelling a live PENDING handle succeeds");
        ok(kiln_stream_cancel(&s, h) == -1,
           "cancelling the SAME handle again is refused — it is already gone");

        KilnStreamHandle h2 = kiln_stream_request(&s, "complete-me", KILN_STREAM_NORMAL, 1.0f, 1, NULL);
        kiln_stream_frame_begin(&s);
        kiln_stream_complete(&s, h2);
        ok(kiln_stream_cancel(&s, h2) == -1,
           "cancelling an already-completed handle is refused, not applied "
           "to whatever now occupies the slot");
    }
}

/* ── kiln_voxel ─────────────────────────────────────────────────────────
 *
 * The two reductions are the whole reason this module exists, and both have
 * failure modes that render plausibly: a greedy box overlapping its neighbour
 * is a collision brush the player sticks inside, and a dropped surface quad is
 * a hole you can see through. Neither is distinguishable from level-design
 * intent in a screenshot. So the properties are asserted as properties —
 * exact tiling, disjointness, determinism — rather than against a golden
 * output that would have to be regenerated every time the scan order changed.
 */

/* The world is ~100 KB; static rather than on the stack. */
static KilnVoxelWorld g_w;

/* Reference face test, deliberately written the slow obvious way: a face is
 * exposed iff the block is solid and its neighbour along the normal is air.
 * The greedy mesher must agree with this everywhere, and having two
 * independent statements of the same fact is the point. */
static int face_exposed_ref(const KilnVoxelWorld *w, int x, int y, int z, int dir)
{
    if (kiln_voxel_get(w, x, y, z) == KILN_VOXEL_AIR) return 0;
    int axes[3], sign;
    kiln_voxel_dir_axes((uint8_t)dir, axes, &sign);
    int p[3] = { x, y, z };
    p[axes[2]] += sign;
    return kiln_voxel_get(w, p[0], p[1], p[2]) == KILN_VOXEL_AIR;
}

static void test_voxel(void)
{
    puts("\nkiln_voxel");

    /* ── Chunk residency ───────────────────────────────────────────────*/
    kiln_voxel_clear(&g_w);
    ok(kiln_voxel_chunk_count(&g_w) == 0, "a cleared world has no chunks");
    ok(kiln_voxel_get(&g_w, 0, 0, 0) == KILN_VOXEL_AIR, "an empty world is air");
    ok(kiln_voxel_get(&g_w, -1, 0, 0) == KILN_VOXEL_AIR,
       "out of bounds reads as air, so neighbour probes need no guard");

    kiln_voxel_set(&g_w, 5, 5, 5, 1);
    ok(kiln_voxel_get(&g_w, 5, 5, 5) == 1, "a set block reads back");
    ok(kiln_voxel_chunk_count(&g_w) == 1, "one block allocates exactly one chunk");

    /* Clearing air where no chunk exists must not allocate: a break aimed at
     * the sky costing a chunk slot would spend the scarce resource on nothing. */
    kiln_voxel_set(&g_w, 200, 40, 200, KILN_VOXEL_AIR);
    ok(kiln_voxel_chunk_count(&g_w) == 1,
       "clearing air in an absent chunk allocates nothing");

    kiln_voxel_set(&g_w, 5, 5, 5, KILN_VOXEL_AIR);
    ok(kiln_voxel_chunk_count(&g_w) == 0,
       "emptying a chunk releases its slot");

    /* Slot exhaustion must be REPORTED. A silently dropped edit in an editor is
     * the worst possible failure: the block simply does not appear and the user
     * assumes they mis-aimed. */
    kiln_voxel_clear(&g_w);
    int refused = 0;
    for (int i = 0; i < KILN_VOXEL_MAX_CHUNKS + 4; i++) {
        /* Walk the chunk grid in 2D. The first version of this walked +X only
         * and ran off the end of a 16-chunk-wide grid at i == 16, where
         * kiln_voxel_set correctly no-ops on an out-of-bounds coordinate — so
         * the test measured the grid width and called it the residency cap. */
        int cx = i % KILN_VOXEL_GRID_X, cz = i / KILN_VOXEL_GRID_X;
        int st = kiln_voxel_set(&g_w, cx * KILN_VOXEL_CHUNK, 0,
                                     cz * KILN_VOXEL_CHUNK, 1);
        if (st == KILN_VOXEL_EFULL) refused++;
    }
    ok(kiln_voxel_chunk_count(&g_w) == KILN_VOXEL_MAX_CHUNKS,
       "residency stops at KILN_VOXEL_MAX_CHUNKS");
    ok(refused == 4, "the 4 edits past the cap each returned EFULL");

    ok(kiln_voxel_set(&g_w, 0, 0, 0, KILN_VOXEL_TYPE_MAX + 1) == KILN_VOXEL_ETYPE,
       "a block type past the 16-entry CI4 palette is refused");

    /* ── Fill ──────────────────────────────────────────────────────────*/
    kiln_voxel_clear(&g_w);
    int n = kiln_voxel_fill(&g_w, 4, 4, 4, 6, 6, 6, 2);
    ok(n == 27, "an inclusive 3x3x3 fill changes 27 blocks");
    ok(kiln_voxel_solid_count(&g_w) == 27, "and the solid count agrees");
    ok(kiln_voxel_fill(&g_w, 4, 4, 4, 6, 6, 6, 2) == 0,
       "re-filling with the same type changes nothing");

    /* Corners in any order: a drag selection has no reason to run +X+Y+Z. */
    kiln_voxel_clear(&g_w);
    ok(kiln_voxel_fill(&g_w, 6, 6, 6, 4, 4, 4, 2) == 27,
       "reversed fill corners are normalised, not rejected");

    /* ── Reduction 1: boxes exactly tile the solid set ─────────────────*/
    kiln_voxel_clear(&g_w);
    kiln_voxel_fill(&g_w, 2, 0, 2, 9, 0, 9, 1);      /* a floor slab      */
    kiln_voxel_fill(&g_w, 2, 1, 2, 2, 3, 9, 2);      /* a wall, other type */
    kiln_voxel_set(&g_w, 7, 2, 7, 3);                /* a lone block      */

    static KilnBrush boxes[512];
    int nb = kiln_voxel_boxes(&g_w, boxes, 512, NULL);
    ok(nb > 0, "boxes were produced (%d)", nb);

    /* Every solid block is covered exactly once, and no air block is covered
     * at all. This is the property that matters: the union is the solid set
     * and the boxes are disjoint. */
    int over = 0, under = 0, phantom = 0;
    for (int z = 0; z < 16; z++)
    for (int y = 0; y < 8; y++)
    for (int x = 0; x < 16; x++) {
        float cx = (float)x * KILN_VOXEL_BLOCK_UNITS + KILN_VOXEL_BLOCK_UNITS * 0.5f;
        float cy = (float)y * KILN_VOXEL_BLOCK_UNITS + KILN_VOXEL_BLOCK_UNITS * 0.5f;
        float cz = (float)z * KILN_VOXEL_BLOCK_UNITS + KILN_VOXEL_BLOCK_UNITS * 0.5f;
        int cover = 0;
        for (int i = 0; i < nb; i++)
            if (cx > boxes[i].mins.v[0] && cx < boxes[i].maxs.v[0] &&
                cy > boxes[i].mins.v[1] && cy < boxes[i].maxs.v[1] &&
                cz > boxes[i].mins.v[2] && cz < boxes[i].maxs.v[2]) cover++;
        uint8_t b = kiln_voxel_get(&g_w, x, y, z);
        if (b != KILN_VOXEL_AIR) {
            if (cover == 0) under++;
            else if (cover > 1) over++;
        } else if (cover > 0) phantom++;
    }
    ok(under == 0, "every solid block is inside some box");
    ok(over == 0, "no block is inside two boxes (disjoint, so no stuck player)");
    ok(phantom == 0, "no air block is inside a box");

    /* A box must not span two block types, or one brush carries two surfaces
     * and the footstep sound is decided by whichever won. */
    int mixed = 0;
    for (int i = 0; i < nb; i++) {
        int bx = (int)(boxes[i].mins.v[0] / KILN_VOXEL_BLOCK_UNITS);
        int by = (int)(boxes[i].mins.v[1] / KILN_VOXEL_BLOCK_UNITS);
        int bz = (int)(boxes[i].mins.v[2] / KILN_VOXEL_BLOCK_UNITS);
        uint8_t t = kiln_voxel_get(&g_w, bx, by, bz);
        if (boxes[i].surface != t) mixed++;
    }
    ok(mixed == 0, "each box's surface id is its blocks' own type");

    /* Determinism: a `.map` export that reorders between runs makes every save
     * look like a change and destroys the diff as a review tool. */
    static KilnBrush boxes2[512];
    int nb2 = kiln_voxel_boxes(&g_w, boxes2, 512, NULL);
    ok(nb2 == nb && memcmp(boxes, boxes2, (size_t)nb * sizeof(KilnBrush)) == 0,
       "the same world yields byte-identical boxes (diffable export)");

    /* The cap contract: report what was needed, write nothing. A caller must be
     * able to size a buffer in one retry rather than by guessing. */
    int tight = kiln_voxel_boxes(&g_w, boxes2, 1, NULL);
    ok(tight == -nb, "an undersized cap returns -(count needed), got %d", tight);

    /* ── Reduction 2: quads match the reference face test ──────────────*/
    kiln_voxel_clear(&g_w);
    kiln_voxel_fill(&g_w, 1, 1, 1, 5, 4, 5, 1);       /* a solid block     */
    kiln_voxel_fill(&g_w, 2, 2, 2, 4, 3, 4, KILN_VOXEL_AIR); /* hollowed out */

    static KilnVoxelQuad quads[4096];
    int slot = kiln_voxel_slot_first(&g_w);
    ok(slot >= 0, "the world has a chunk to mesh");
    int nq = kiln_voxel_quads(&g_w, slot, quads, 4096);
    ok(nq > 0, "quads were produced (%d)", nq);

    /* Total quad AREA must equal the number of exposed faces. Comparing areas
     * rather than counts is what makes this independent of how greedily the
     * mesher merged — it may emit one 4x4 quad or sixteen 1x1, but it may not
     * emit 15 or 17 faces' worth. */
    long area = 0;
    for (int i = 0; i < nq; i++) area += (long)quads[i].w * quads[i].h;

    long ref = 0;
    for (int z = 0; z < KILN_VOXEL_CHUNK; z++)
    for (int y = 0; y < KILN_VOXEL_CHUNK; y++)
    for (int x = 0; x < KILN_VOXEL_CHUNK; x++)
        for (int d = 0; d < KILN_VOXEL_DIRS; d++)
            if (face_exposed_ref(&g_w, x, y, z, d)) ref++;

    ok(area == ref, "merged quad area == exposed faces (%ld vs %ld)", area, ref);

    /* And no quad may cover an unexposed face, which area alone cannot catch:
     * one missing face plus one spurious face is the same total. */
    int spurious = 0;
    for (int i = 0; i < nq; i++) {
        int axes[3], sign;
        kiln_voxel_dir_axes(quads[i].dir, axes, &sign);
        for (int v = 0; v < quads[i].h; v++)
        for (int u = 0; u < quads[i].w; u++) {
            int p[3] = { quads[i].x, quads[i].y, quads[i].z };
            p[axes[0]] += u;
            p[axes[1]] += v;
            if (!face_exposed_ref(&g_w, p[0], p[1], p[2], quads[i].dir)) spurious++;
        }
    }
    ok(spurious == 0, "no quad covers a face that is not exposed");

    /* A chunk seam must not become a wall. Two chunks solid across the boundary
     * means the faces on the seam are NOT exposed — meshing a chunk against its
     * own array instead of the world gets this wrong, and the result reads as
     * "the level is made of boxes", which would be true. */
    kiln_voxel_clear(&g_w);
    kiln_voxel_fill(&g_w, KILN_VOXEL_CHUNK - 2, 0, 0, KILN_VOXEL_CHUNK + 1, 0, 0, 1);
    long seam_area = 0;
    for (int s = kiln_voxel_slot_first(&g_w); s >= 0; s = kiln_voxel_slot_next(&g_w, s)) {
        int c = kiln_voxel_quads(&g_w, s, quads, 4096);
        for (int i = 0; i < c; i++)
            if (quads[i].dir == KILN_VOXEL_XP || quads[i].dir == KILN_VOXEL_XN)
                seam_area += (long)quads[i].w * quads[i].h;
    }
    ok(seam_area == 2, "a 4-long bar across a chunk seam has 2 X faces, not 4");

    /* Editing next to a seam must dirty the neighbour too, or its mesh keeps a
     * face where air now is. */
    kiln_voxel_clear(&g_w);
    kiln_voxel_set(&g_w, KILN_VOXEL_CHUNK, 0, 0, 1);       /* chunk 1 */
    kiln_voxel_set(&g_w, KILN_VOXEL_CHUNK - 1, 0, 0, 1);   /* chunk 0, on the seam */
    int both_dirty = 1;
    for (int s = kiln_voxel_slot_first(&g_w); s >= 0; s = kiln_voxel_slot_next(&g_w, s))
        if (!g_w.chunks[s].dirty) both_dirty = 0;
    ok(both_dirty, "an edit on a seam marks the neighbouring chunk dirty");

    /* ── Raycast ───────────────────────────────────────────────────────*/
    kiln_voxel_clear(&g_w);
    kiln_voxel_set(&g_w, 10, 2, 2, 1);

    const float B = (float)KILN_VOXEL_BLOCK_UNITS;
    KilnVoxelHit h;
    fm_vec3_t o = {{ 2.5f * B, 2.5f * B, 2.5f * B }};
    fm_vec3_t d = {{ 1.0f, 0.0f, 0.0f }};
    ok(kiln_voxel_raycast(&g_w, &o, &d, 100.0f * B, &h) == 1, "the ray hits");
    ok(h.x == 10 && h.y == 2 && h.z == 2, "and hits the right block");
    ok(h.nx == -1 && h.ny == 0 && h.nz == 0,
       "the normal faces back along the ray");
    ok(h.px == 9 && h.py == 2 && h.pz == 2,
       "the place-here cell is in FRONT of the face, not inside the wall");
    ok(h.dir == KILN_VOXEL_XN, "the face direction is -X");

    /* The distance must be the real one: 7.5 blocks from x=2.5 to the x=10
     * boundary. A fraction that is 0.999 instead of 1 is exactly what a
     * screenshot cannot tell you. */
    ok(fabsf(h.dist - 7.5f * B) < 0.01f,
       "the hit distance is 7.5 blocks (%f)", (double)h.dist / B);

    /* Range must be respected, or the reticle grabs blocks across the map. */
    ok(kiln_voxel_raycast(&g_w, &o, &d, 3.0f * B, &h) == 0,
       "a ray shorter than the gap does not hit");

    /* An unnormalised direction must behave identically — callers pass a
     * camera forward vector and will not always normalise it. */
    fm_vec3_t d2 = {{ 17.0f, 0.0f, 0.0f }};
    KilnVoxelHit h2;
    /* Compared against the KNOWN 7.5 blocks, not against `h` — the range test
     * just above missed, and a miss still zeroes the out struct (deliberately),
     * so `h.dist` is 0 by this point. Comparing two results where one has been
     * invalidated is a test that passes for the wrong reason just as easily. */
    ok(kiln_voxel_raycast(&g_w, &o, &d2, 100.0f * B, &h2) == 1 &&
       h2.x == 10 && fabsf(h2.dist - 7.5f * B) < 0.01f,
       "an unnormalised direction gives the same hit and distance");

    /* Firing from inside a solid block reports it rather than skipping it:
     * otherwise a block placed on top of the camera is unbreakable. */
    fm_vec3_t inside = {{ 10.5f * B, 2.5f * B, 2.5f * B }};
    ok(kiln_voxel_raycast(&g_w, &inside, &d, 100.0f * B, &h) == 1 && h.dist == 0.0f,
       "a ray starting inside a block reports it at distance 0");

    /* Fired from OUTSIDE the grid, looking in. This is the normal case for an
     * editor camera and it is where the DDA's exit test is easiest to get
     * wrong: bailing on the first out-of-bounds cell instead of on the first
     * cell from which the grid is unreachable makes every such ray miss, and
     * the symptom is a reticle that reports nothing while a room fills the
     * screen. */
    fm_vec3_t outside = {{ -6.0f * B, 2.5f * B, 2.5f * B }};
    ok(kiln_voxel_raycast(&g_w, &outside, &d, 100.0f * B, &h) == 1 &&
       h.x == 10 && h.y == 2 && h.z == 2,
       "a ray fired from outside the grid still hits a block inside it");

    /* And a diagonal approach from outside on two axes at once, since the exit
     * test is per-axis and only the stepped axis is checked. */
    fm_vec3_t diag_o = {{ -4.0f * B, -3.0f * B, 2.5f * B }};
    fm_vec3_t diag_d = {{ 14.0f, 5.0f, 0.0f }};
    ok(kiln_voxel_raycast(&g_w, &diag_o, &diag_d, 400.0f * B, &h) == 1,
       "a ray approaching from outside on two axes still enters the grid");

    /* A ray heading AWAY from the grid must give up rather than walk the guard
     * loop to its limit. */
    fm_vec3_t away = {{ -6.0f * B, 2.5f * B, 2.5f * B }};
    fm_vec3_t away_d = {{ -1.0f, 0.0f, 0.0f }};
    ok(kiln_voxel_raycast(&g_w, &away, &away_d, 1000.0f * B, &h) == 0,
       "a ray pointing away from the grid misses");

    /* A ray into empty space must terminate, not walk the guard loop. */
    kiln_voxel_clear(&g_w);
    ok(kiln_voxel_raycast(&g_w, &o, &d, 1000.0f * B, &h) == 0,
       "a ray through an empty world misses");

    /* Exactly axis-parallel components are the classic DDA divide-by-zero. */
    fm_vec3_t dz = {{ 0.0f, 0.0f, 1.0f }};
    kiln_voxel_set(&g_w, 2, 2, 9, 4);
    ok(kiln_voxel_raycast(&g_w, &o, &dz, 100.0f * B, &h) == 1 && h.z == 9,
       "an axis-parallel ray does not divide by zero");

    /* ── Bounds ────────────────────────────────────────────────────────*/
    kiln_voxel_clear(&g_w);
    int mins[3], maxs[3];
    ok(kiln_voxel_bounds(&g_w, mins, maxs) == 0, "an empty world has no bounds");
    kiln_voxel_set(&g_w, 3, 4, 5, 1);
    kiln_voxel_set(&g_w, 20, 6, 7, 1);
    ok(kiln_voxel_bounds(&g_w, mins, maxs) == 1 &&
       mins[0] == 3 && mins[1] == 4 && mins[2] == 5 &&
       maxs[0] == 20 && maxs[1] == 6 && maxs[2] == 7,
       "bounds are the inclusive AABB over solid blocks");
}

int main(void)
{
    test_clip();
    test_dict();
    test_cache();
    test_lod();
    test_rng();
    test_stream();
    test_voxel();

    putchar('\n');
    if (g_fail) {
        printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    puts("all engine-logic checks passed");
    return 0;
}
