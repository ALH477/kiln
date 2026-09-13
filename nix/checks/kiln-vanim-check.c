/* SPDX-License-Identifier: MIT
 *
 * kiln_morph_update's blend, asserted on the host with the real kiln_vanim.c.
 *
 * The blend is CPU arithmetic over T3DVertPacked and needs no RSP, so it can
 * be checked here even though kiln_morph_init/draw cannot (vertex placeholders
 * abort on the host). The KilnMorph is assembled by hand around a model that
 * only states its vertex count — exactly the fields the update reads.
 *
 * What it pins, each of which rendered on console with nothing failing:
 *
 *   colour     RGBA8 is four 8-bit channels packed in a uint32. Scaling the
 *              packed word by a float weight carries between channels: a
 *              50/50 blend of pure red and pure green came out as a muddy
 *              brown-orange instead of (127,127,0), and most blends garbled.
 *   normals    the work buffer was memset to zero and normals were never
 *              written, although the comment said "take the first target's".
 *              A zero normal lights nothing, so a morphing mesh rendered at
 *              flat ambient. Then they came from the heaviest target alone,
 *              and the lighting jumped at the 50% crossover of every morph;
 *              now they blend between the two heaviest, and a held shape
 *              keeps its target's normal bit for bit.
 *   rounding   each target's contribution was truncated separately, so a
 *              blend of three identical targets lost up to three units.
 *   weights    the sum was taken over CLAMPED weights but the loop divided the
 *              raw ones, so an out-of-range weight overshot the shape.
 *   ping-pong  successive updates write alternate buffers, so the RSP never
 *              reads a buffer the CPU is rewriting.
 *   deform     kiln_deform_update starts every frame from the base copy — a
 *              callback that ADDS a displacement must not compound — writes
 *              alternate buffers, and never touches the base.
 */
#include <kiln_vanim.h>
#include <libdragon.h>
#include <t3d/t3d.h>

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int ch(uint32_t rgba, int shift) { return (int)((rgba >> shift) & 0xFF); }

/* Packed unit normals, spelled out: 5.6.5 signed, x and z scaled 15.5, y 31.5. */
#define N_PX 0x7800   /* (+1, 0, 0): x = 15            */
#define N_PZ 0x000F   /* (0, 0, +1): z = 15            */
#define N_PY 0x03E0   /* (0, +1, 0): y = 31            */
#define N_NY 0x0400   /* (0, -1, 0): y = -32           */
#define N_XZ 0x580B   /* (0.707, 0, 0.707): x = z = 11 */

static int deform_calls;
static float deform_last_time;
static void push_x(T3DVertPacked *verts, int count, float time, void *user)
{
    (void)user;
    deform_calls++;
    deform_last_time = time;
    for (int i = 0; i < (count + 1) / 2; i++) verts[i].posA[0] += 10;
}

int main(void)
{
    /* Two packed entries = four vertices. */
    static T3DModel model;
    model.totalVertCount = 4;

    static T3DVertPacked red[2], green[2], blue[2], work[4];
    for (int i = 0; i < 2; i++) {
        red[i]   = (T3DVertPacked){ .posA = { 100, 0, -40 }, .normA = N_PX,
                                    .posB = { 7, 7, 7 },      .normB = N_PY,
                                    .rgbaA = 0xFF0000FF, .rgbaB = 0xFF0000FF,
                                    .stA = { 64, 0 }, .stB = { 0, 64 } };
        green[i] = (T3DVertPacked){ .posA = { 0, 100, -40 }, .normA = N_PZ,
                                    .posB = { 7, 7, 7 },      .normB = N_NY,
                                    .rgbaA = 0x00FF00FF, .rgbaB = 0x00FF00FF,
                                    .stA = { 0, 64 }, .stB = { 64, 0 } };
        blue[i]  = (T3DVertPacked){ .posA = { 0, 0, 100 },   .normA = 0x5555,
                                    .posB = { 7, 7, 7 },      .normB = 0xAAAA,
                                    .rgbaA = 0x0000FFFF, .rgbaB = 0x0000FFFF,
                                    .stA = { 0, 0 }, .stB = { 0, 0 } };
    }
    T3DVertPacked *targets[3] = { red, green, blue };
    float weights[3] = { 0.5f, 0.5f, 0.0f };

    KilnMorph m = {
        .model = &model, .targets = targets, .target_count = 3,
        .weights = weights, .work_buffers = work, .buffer_count = 2,
        .current_buffer = 0, .segment_id = 1, .initialised = true,
    };

    /* ── 50/50 red + green ─────────────────────────────────────────── */
    kiln_morph_update(&m, 1.0f / 60.0f);
    const T3DVertPacked *d = &work[0];
    CHECK(ch(d->rgbaA, 24) >= 126 && ch(d->rgbaA, 24) <= 128 &&
          ch(d->rgbaA, 16) >= 126 && ch(d->rgbaA, 16) <= 128 &&
          ch(d->rgbaA, 8) == 0 && ch(d->rgbaA, 0) == 255,
          "red+green at 0.5 each is (%d,%d,%d,%d), want (127,127,0,255)",
          ch(d->rgbaA, 24), ch(d->rgbaA, 16), ch(d->rgbaA, 8), ch(d->rgbaA, 0));
    CHECK(d->posA[0] == 50 && d->posA[1] == 50 && d->posA[2] == -40,
          "position blend is (%d,%d,%d), want (50,50,-40)",
          d->posA[0], d->posA[1], d->posA[2]);
    CHECK(d->posB[0] == 7 && d->posB[1] == 7 && d->posB[2] == 7,
          "blending identical positions gives (%d,%d,%d), want (7,7,7)",
          d->posB[0], d->posB[1], d->posB[2]);
    CHECK(d->normA != 0 && d->normB != 0,
          "normals are 0x%04x/0x%04x after the blend; a zero normal lights nothing",
          d->normA, d->normB);
    CHECK(d->normA == N_XZ,
          "+X and +Z at 0.5 each packs to 0x%04x, want 0x%04x (0.707, 0, 0.707)", d->normA, N_XZ);
    CHECK(d->normB == N_PY,
          "opposite normals cancel; the blend kept 0x%04x, want the heavier target's 0x%04x",
          d->normB, N_PY);
    CHECK(d->stA[0] == 32 && d->stA[1] == 32, "uv blend is (%d,%d), want (32,32)",
          d->stA[0], d->stA[1]);

    /* ── three identical targets: nothing may be lost to rounding ───── */
    static T3DVertPacked same[2];
    for (int i = 0; i < 2; i++)
        same[i] = (T3DVertPacked){ .posA = { 101, -101, 33 }, .normA = 0x0101,
                                   .posB = { 1, 2, 3 }, .normB = 0x0101,
                                   .rgbaA = 0x80C0E0FF, .rgbaB = 0x10203040 };
    T3DVertPacked *same3[3] = { same, same, same };
    float thirds[3] = { 1.0f, 1.0f, 1.0f };
    m.targets = same3; m.weights = thirds;
    kiln_morph_update(&m, 1.0f / 60.0f);
    d = &work[2];                       /* the second buffer this time */
    CHECK(d->posA[0] == 101 && d->posA[1] == -101 && d->posA[2] == 33,
          "three identical targets blend to (%d,%d,%d), want (101,-101,33)",
          d->posA[0], d->posA[1], d->posA[2]);
    CHECK(d->rgbaA == 0x80C0E0FF && d->rgbaB == 0x10203040,
          "three identical colours blend to %08x/%08x", d->rgbaA, d->rgbaB);

    /* ── ping-pong: the two updates wrote different buffers ────────── */
    CHECK(work[0].posA[0] == 50 && work[2].posA[0] == 101,
          "updates did not alternate buffers (buf0 x=%d, buf1 x=%d)",
          work[0].posA[0], work[2].posA[0]);
    CHECK(m.current_buffer == 0, "after two updates the next buffer is %d, want 0",
          m.current_buffer);

    /* ── an out-of-range weight is clamped, not used raw ───────────── */
    float over[3] = { 3.0f, 1.0f, 0.0f };
    m.targets = targets; m.weights = over;
    kiln_morph_update(&m, 1.0f / 60.0f);
    d = &work[0];
    CHECK(d->posA[0] == 50 && d->posA[1] == 50,
          "weights {3,1,0} blend to (%d,%d); clamped they are {1,1,0} -> (50,50)",
          d->posA[0], d->posA[1]);

    /* ── a held shape keeps its target's normal exactly ─────────────── */
    float held[3] = { 0.98f, 0.02f, 0.0f };
    m.weights = held;
    kiln_morph_update(&m, 1.0f / 60.0f);
    d = &work[2];
    CHECK(d->normA == N_PX,
          "weights {0.98,0.02} give normal 0x%04x; under a 5%% share it is the dominant 0x%04x",
          d->normA, N_PX);
    /* and past 5% it moves off it: no jump at the crossover */
    float leaning[3] = { 0.7f, 0.3f, 0.0f };
    m.weights = leaning;
    kiln_morph_update(&m, 1.0f / 60.0f);
    d = &work[0];
    CHECK(d->normA != N_PX && d->normA != N_PZ,
          "weights {0.7,0.3} give normal 0x%04x, which is one target's; it should be between", d->normA);

    /* ── kiln_deform ───────────────────────────────────────────────── */
    static T3DModel dmodel;
    dmodel.totalVertCount = 4;
    static T3DVertPacked base[2], dwork[4];
    base[0] = (T3DVertPacked){ .posA = { 5, 1, 2 } };
    base[1] = (T3DVertPacked){ .posA = { -5, 1, 2 } };
    KilnDeform df = {
        .model = &dmodel, .fn = push_x, .work_buffers = dwork, .base_buffer = base,
        .vert_count = 4, .buffer_count = 2, .current_buffer = 0, .segment_id = 1,
        .initialised = true,
    };
    kiln_deform_update(&df, 0.25f);
    kiln_deform_update(&df, 0.25f);
    CHECK(dwork[0].posA[0] == 15 && dwork[2].posA[0] == 15,
          "each frame starts from the base: buffers hold x = %d and %d, want 15 and 15 (not 25)",
          dwork[0].posA[0], dwork[2].posA[0]);
    CHECK(base[0].posA[0] == 5 && base[1].posA[0] == -5,
          "the base copy was modified (x = %d, %d)", base[0].posA[0], base[1].posA[0]);
    CHECK(deform_calls == 2 && deform_last_time == 0.5f,
          "callback ran %d times, last with t = %.2f; want 2 and 0.50", deform_calls, (double)deform_last_time);
    CHECK(df.current_buffer == 0, "after two updates the next buffer is %d, want 0", df.current_buffer);

    if (fails) { printf("\nFAILED (%d)\n", fails); return 1; }
    printf("kiln_morph: per-channel colour, blended normals, exact rounding, clamped weights, alternating buffers\n");
    printf("kiln_deform: starts from base each frame, alternating buffers, base untouched\n");
    return 0;
}
