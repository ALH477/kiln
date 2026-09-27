/* SPDX-License-Identifier: MIT
 *
 * fig_pose_parity.c — the Exsecutor core against the C it replaces.
 *
 * engine/src/kiln/fig_pose.exsc is Figulina's bone arithmetic written once,
 * emitted per row, and meant to produce the same bits on the VR4300 as on
 * x86-64. That is a claim, and this is what makes it falsifiable: it links
 * the EMITTED core beside a transcription of kiln_pose.c's arithmetic and
 * compares them.
 *
 * ── Why the comparison is bit-exact and not approximate ────────────────
 * fig_quat_mul is four multiplies and three add/subs per lane — no division,
 * no root, no reassociation the compiler is allowed to perform on IEEE
 * floats. So the right tolerance is ZERO, and memcmp is the right operator.
 * A tolerance here would hide precisely the divergence the file exists to
 * catch: `-ffast-math` on one side, a contracted multiply-add on the other,
 * or an emitted expression that reassociated.
 *
 * ── Why subtree_mask is tested against shapes, not random bits ─────────
 * The mask means "this bone and the run of following bones deeper than it",
 * which is only meaningful over a DEPTH-FIRST column. Random uint16s would
 * exercise the early-out and nothing else. The generator walks depth by
 * -1/0/+1 so the columns are the shapes a real skeleton produces, and every
 * root in range is tried against every column.
 *
 * ── How to run it ──────────────────────────────────────────────────────
 * Against the emitted C for this row:
 *
 *     cc -std=c11 -O2 -fsanitize=undefined -fno-sanitize-recover=all \
 *        -o parity fig_pose_parity.c ../gen/fig_pose_x86_64.gen.c -lm
 *
 * Exsecutor's own rig requires byte-identical output from gcc and clang at
 * -O0 and -O2; this file is written to be run the same four ways, because a
 * core that agrees under one compiler and not another has not been shown to
 * agree with anything.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

uint64_t exs_fig_quat_mul(unsigned char *o, unsigned char *a, unsigned char *b);
uint64_t exs_fig_pose_subtree_mask(unsigned char *d, uint64_t n, uint64_t root);
_Noreturn void exsrt_abortus(unsigned k) { fprintf(stderr, "abortus %u\n", k); _Exit(70); }

#define MAXB 32
static void c_quat_mul(float *o, const float *a, const float *b) {
    const float ax=a[0],ay=a[1],az=a[2],aw=a[3];
    const float bx=b[0],by=b[1],bz=b[2],bw=b[3];
    o[0] = aw*bx + ax*bw + ay*bz - az*by;
    o[1] = aw*by - ax*bz + ay*bw + az*bx;
    o[2] = aw*bz + ax*by - ay*bx + az*bw;
    o[3] = aw*bw - ax*bx - ay*by - az*bz;
}
static uint32_t c_subtree(const uint16_t *depth, int n, int root) {
    if (root < 0 || root >= n) return 0;
    uint32_t mask = 0;
    for (int j = root; j < n; j++) {
        if (j > root && depth[j] <= depth[root]) break;
        if (j < MAXB) mask |= 1u << j;
    }
    return mask;
}

static uint32_t rng = 0x12345678u;
static float rf(void){ rng = rng*1664525u + 1013904223u; return (float)((int32_t)rng) / 2147483648.0f; }

int main(void) {
    int bad = 0, n_mul = 0, n_mask = 0;

    /* ---- quat_mul: random quaternions, BIT-exact comparison ---- */
    for (int it = 0; it < 20000; it++) {
        float a[4], b[4], co[4], eo[4];
        for (int k = 0; k < 4; k++) { a[k] = rf(); b[k] = rf(); }
        c_quat_mul(co, a, b);
        memset(eo, 0, sizeof eo);
        exs_fig_quat_mul((unsigned char*)eo, (unsigned char*)a, (unsigned char*)b);
        if (memcmp(co, eo, sizeof co) != 0) {
            if (bad < 3) printf("  MUL MISMATCH it=%d  C=[%a %a %a %a]  EXS=[%a %a %a %a]\n",
                   it, co[0],co[1],co[2],co[3], eo[0],eo[1],eo[2],eo[3]);
            bad++;
        }
        n_mul++;
    }

    /* ---- subtree_mask: exhaustive over shapes the engine can produce ---- */
    for (int trial = 0; trial < 4000; trial++) {
        uint16_t d[MAXB];
        int n = 1 + (int)(rng % MAXB); rng = rng*1664525u + 1013904223u;
        d[0] = 0;
        for (int j = 1; j < MAXB; j++) {
            rng = rng*1664525u + 1013904223u;
            int step = (int)(rng % 3) - 1;              /* -1, 0, +1 : depth-first shape */
            int v = (int)d[j-1] + step; if (v < 0) v = 0; if (v > 8) v = 8;
            d[j] = (uint16_t)v;
        }
        for (int root = 0; root < n; root++) {
            uint32_t c = c_subtree(d, n, root);
            uint32_t e = (uint32_t)exs_fig_pose_subtree_mask((unsigned char*)d, (uint64_t)n, (uint64_t)root);
            if (c != e) {
                if (bad < 6) printf("  MASK MISMATCH n=%d root=%d  C=%08x EXS=%08x\n", n, root, c, e);
                bad++;
            }
            n_mask++;
        }
    }
    printf("quat_mul: %d cases bit-exact-compared\nsubtree_mask: %d cases compared\n", n_mul, n_mask);
    if (bad) { printf("FAILED: %d mismatches\n", bad); return 1; }
    printf("AGREE: the Exsecutor core matches kiln_pose.c on every case\n");
    return 0;
}
