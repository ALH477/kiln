/* SPDX-License-Identifier: MIT
 *
 * live_render.c — drive a LIVE Faust voice (dsp/arch/libdragon_mixer.c, built
 * -single -os) on the host and write what it produces as a WAV.
 *
 * The offline renderer (dsp/arch/offline_ref.c) proves an instrument sounds right
 * at full quality. It says nothing about the voice that is actually linked into
 * a ROM: that one is single precision, one sample per call, flush-to-zero, and
 * reaches its parameters through the architecture's zone table. Every one of those
 * is a place a voice can differ from its own reference — a decay time that was
 * fine in double and stalls in float, a gate read once per block instead of
 * once per sample. This renders the real thing and lets analyse.py hold it to
 * the same numbers.
 *
 * Built twice per voice by nix/instruments.nix and tools/instruments/dev.sh:
 * once with the voice's translation unit (faust output + architecture) and
 * -DFAUST_NAME=<name>; this file declares the architecture's three public
 * entry points by their mangled names and calls them. It is deliberately NOT
 * the console build: it is the same C, the host's compiler.
 *
 * usage: live_render -o out.wav [-r rate] [-d seconds] [-p label=value ...] [-g label:on:off]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define SYM(s) CAT(CAT(faust_n64_, FAUST_NAME), s)

extern void SYM(_init)(int sample_rate);
extern float *SYM(_param)(const char *label);
extern void SYM(_render)(int16_t *out, int nframes, int accumulate);

static void put32(FILE *f, uint32_t v) { for (int i = 0; i < 4; i++) fputc((v >> (8 * i)) & 255, f); }
static void put16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc(v >> 8, f); }

int main(int argc, char **argv)
{
    const char *out = NULL;
    int rate = 32000;
    double dur = 2.0;
    const char *sets[64];
    int nsets = 0;
    const char *gate = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) dur = atof(argv[++i]);
        else if (!strcmp(argv[i], "-p") && i + 1 < argc && nsets < 64) sets[nsets++] = argv[++i];
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) gate = argv[++i];
        else { fprintf(stderr, "usage: %s -o out.wav [-r rate] [-d s] [-p k=v] [-g label:on:off]\n", argv[0]); return 2; }
    }
    if (!out) return 2;

    SYM(_init)(rate);
    for (int i = 0; i < nsets; i++) {
        char buf[128];
        snprintf(buf, sizeof buf, "%s", sets[i]);
        char *eq = strchr(buf, '=');
        if (!eq) return 2;
        *eq = 0;
        float *z = SYM(_param)(buf);
        if (!z) { fprintf(stderr, "no such parameter '%s'\n", buf); return 2; }
        *z = (float)atof(eq + 1);
    }
    float *gz = NULL;
    double on = 0, off = 0;
    if (gate) {
        char buf[128];
        snprintf(buf, sizeof buf, "%s", gate);
        char *c1 = strchr(buf, ':');
        char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
        if (!c2) return 2;
        *c1 = *c2 = 0;
        gz = SYM(_param)(buf);
        if (!gz) { fprintf(stderr, "no gate '%s'\n", buf); return 2; }
        on = atof(c1 + 1);
        off = atof(c2 + 1);
    }

    int total = (int)(dur * rate);
    FILE *f = fopen(out, "wb");
    if (!f) return 1;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + total * 2); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, 1); put32(f, rate); put32(f, rate * 2); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, total * 2);

    /* One frame at a time, so the gate changes on the exact sample, which is
     * what a ROM does between audio buffers of 1 frame to 512; the voice must
     * not depend on the gate being block-aligned. */
    int16_t pair[2];
    for (int n = 0; n < total; n++) {
        if (gz) {
            double t = (double)n / rate;
            *gz = (t >= on && t < off) ? 1.0f : 0.0f;
        }
        SYM(_render)(pair, 1, 0);
        put16(f, (uint16_t)pair[0]);
    }
    fclose(f);
    return 0;
}
