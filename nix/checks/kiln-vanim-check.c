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
 *              flat ambient.
 *   rounding   each target's contribution was truncated separately, so a
 *              blend of three identical targets lost up to three units.
 *   weights    the sum was taken over CLAMPED weights but the loop divided the
 *              raw ones, so an out-of-range weight overshot the shape.
 *   ping-pong  successive updates write alternate buffers, so the RSP never
 *              reads a buffer the CPU is rewriting.
 */
#include <kiln_vanim.h>
#include <libdragon.h>

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int ch(uint32_t rgba, int shift) { return (int)((rgba >> shift) & 0xFF); }

int main(void)
{
    /* Two packed entries = four vertices. */
    static T3DModel model;
    model.totalVertCount = 4;

    static T3DVertPacked red[2], green[2], blue[2], work[4];
    for (int i = 0; i < 2; i++) {
        red[i]   = (T3DVertPacked){ .posA = { 100, 0, -40 }, .normA = 0x1234,
                                    .posB = { 7, 7, 7 },      .normB = 0x0F0F,
                                    .rgbaA = 0xFF0000FF, .rgbaB = 0xFF0000FF,
                                    .stA = { 64, 0 }, .stB = { 0, 64 } };
        green[i] = (T3DVertPacked){ .posA = { 0, 100, -40 }, .normA = 0x4321,
                                    .posB = { 7, 7, 7 },      .normB = 0xF0F0,
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
    CHECK(d->normA == 0x1234 || d->normA == 0x4321,
          "normal 0x%04x is neither target's", d->normA);
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

    if (fails) { printf("\nFAILED (%d)\n", fails); return 1; }
    printf("kiln_morph: per-channel colour, kept normals, exact rounding, clamped weights, alternating buffers\n");
    return 0;
}
