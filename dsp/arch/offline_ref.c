/* SPDX-License-Identifier: MIT
 *
 * dsp/arch/offline_ref.c — host-side Faust renderer, full quality.
 *
 * Written from scratch against Faust's public C ABI (CInterface.h) rather than
 * derived from GRAME's architecture files.
 *
 * This is the OPPOSITE end of the quality spectrum from libdragon_mixer.c, and
 * deliberately so. It runs on the development host with no cycle budget at all,
 * so it is compiled `-double -ftz 2` — the DeMoD house style, and what report
 * §5 prescribes for baking ("Render your Faust PM instruments offline ... at
 * -double -ftz 2, full quality"). It serves two purposes:
 *
 *   1. mkBakedInstrument (report Stage 1): render an instrument to WAV, then
 *      audioconv64 it to VADPCM. This is meant to carry 80-90% of a game's
 *      audio — the N64's best-sounding titles worked this way.
 *   2. The golden reference for A/B validation (report Stage 3): when an inner
 *      loop is hand-ported to RSP fixed point, this is the output it must be
 *      checked against.
 *
 * Block mode is used here (compute(), not frame()) — there is no reason to pay
 * per-sample call overhead offline, and unlike the on-console path we are not
 * interleaving control data between samples.
 *
 * Output is 16-bit PCM WAV: it is what audioconv64 consumes, and VADPCM is a
 * 4-bit ADPCM variant so nothing is gained by handing it more than 16 bits.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "faust/gui/CInterface.h"

#ifndef FAUST_N64_MAX_PARAMS
#define FAUST_N64_MAX_PARAMS 64
#endif

#ifndef FAUST_NAME
#define FAUST_NAME mydsp
#endif

#define FAUST_CAT2(a, b) a##b
#define FAUST_CAT(a, b) FAUST_CAT2(a, b)

#define FAUSTDSP FAUST_NAME
#define FAUST_NEW FAUST_CAT(new, FAUST_NAME)
#define FAUST_DELETE FAUST_CAT(delete, FAUST_NAME)
#define FAUST_INIT FAUST_CAT(init, FAUST_NAME)
#define FAUST_BUILD_UI FAUST_CAT(buildUserInterface, FAUST_NAME)
#define FAUST_COMPUTE FAUST_CAT(compute, FAUST_NAME)
#define FAUST_NUM_OUTPUTS FAUST_CAT(getNumOutputs, FAUST_NAME)
#define FAUST_NUM_INPUTS FAUST_CAT(getNumInputs, FAUST_NAME)

#ifndef max
#define max(a, b) ((a) < (b) ? (b) : (a))
#endif
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

<<includeIntrinsic>>

<<includeclass>>

/* ── Parameter collection ─────────────────────────────────────────────── */

typedef struct {
    const char *label;
    FAUSTFLOAT *zone;
} param_t;

typedef struct {
    param_t params[FAUST_N64_MAX_PARAMS];
    int n;
} ui_t;

static void ui_box(void *u, const char *l) { (void)u; (void)l; }
static void ui_close(void *u) { (void)u; }
static void ui_declare(void *u, FAUSTFLOAT *z, const char *k, const char *v)
{
    (void)u; (void)z; (void)k; (void)v;
}

static void ui_add(void *u, const char *label, FAUSTFLOAT *zone)
{
    ui_t *self = (ui_t *)u;
    if (self->n >= FAUST_N64_MAX_PARAMS) return;
    self->params[self->n].label = label;
    self->params[self->n].zone = zone;
    self->n++;
}

static void ui_button(void *u, const char *l, FAUSTFLOAT *z) { ui_add(u, l, z); }
static void ui_slider(void *u, const char *l, FAUSTFLOAT *z, FAUSTFLOAT i,
                      FAUSTFLOAT lo, FAUSTFLOAT hi, FAUSTFLOAT st)
{
    (void)i; (void)lo; (void)hi; (void)st;
    ui_add(u, l, z);
}
static void ui_bargraph(void *u, const char *l, FAUSTFLOAT *z, FAUSTFLOAT lo, FAUSTFLOAT hi)
{
    (void)lo; (void)hi;
    ui_add(u, l, z);
}
static void ui_soundfile(void *u, const char *l, const char *url, struct Soundfile **s)
{
    (void)u; (void)l; (void)url; (void)s;
}

static void build_ui(FAUSTDSP *dsp, ui_t *out)
{
    UIGlue g;
    memset(out, 0, sizeof(*out));
    g.uiInterface = out;
    g.openTabBox = ui_box;
    g.openHorizontalBox = ui_box;
    g.openVerticalBox = ui_box;
    g.closeBox = ui_close;
    g.addButton = ui_button;
    g.addCheckButton = ui_button;
    g.addVerticalSlider = ui_slider;
    g.addHorizontalSlider = ui_slider;
    g.addNumEntry = ui_slider;
    g.addHorizontalBargraph = ui_bargraph;
    g.addVerticalBargraph = ui_bargraph;
    g.addSoundfile = ui_soundfile;
    g.declare = ui_declare;
    FAUST_BUILD_UI(dsp, &g);
}

static FAUSTFLOAT *find_param(ui_t *ui, const char *label)
{
    for (int i = 0; i < ui->n; i++) {
        if (strcmp(ui->params[i].label, label) == 0) return ui->params[i].zone;
    }
    return NULL;
}

/* ── WAV output ───────────────────────────────────────────────────────── */

static void put_u32(FILE *f, uint32_t v)
{
    fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f);
    fputc((v >> 16) & 0xff, f); fputc((v >> 24) & 0xff, f);
}
static void put_u16(FILE *f, uint16_t v)
{
    fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f);
}

static void write_wav_header(FILE *f, int rate, int channels, int nframes)
{
    const int bits = 16;
    const int data_bytes = nframes * channels * (bits / 8);
    fwrite("RIFF", 1, 4, f);
    put_u32(f, 36 + data_bytes);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    put_u32(f, 16);                                   /* PCM fmt chunk size */
    put_u16(f, 1);                                    /* PCM               */
    put_u16(f, (uint16_t)channels);
    put_u32(f, (uint32_t)rate);
    put_u32(f, (uint32_t)(rate * channels * bits / 8)); /* byte rate       */
    put_u16(f, (uint16_t)(channels * bits / 8));      /* block align       */
    put_u16(f, (uint16_t)bits);
    fwrite("data", 1, 4, f);
    put_u32(f, (uint32_t)data_bytes);
}

/* ── main ─────────────────────────────────────────────────────────────── */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s -o out.wav [-r rate] [-d seconds]\n"
        "          [-p label=value ...] [-g label:on:off]\n"
        "  -p  set a parameter for the whole render\n"
        "  -g  hold a gate/button at 1 from time 'on' to time 'off' (seconds)\n",
        argv0);
}

#define BLOCK 512

int main(int argc, char **argv)
{
    const char *out_path = NULL;
    int rate = 32000;
    double duration = 2.0;

    /* Deferred until after the DSP exists, since zones are only known then. */
    const char *pset[FAUST_N64_MAX_PARAMS];
    int npset = 0;
    const char *gate_spec = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) duration = atof(argv[++i]);
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            if (npset < FAUST_N64_MAX_PARAMS) pset[npset++] = argv[++i];
        } else if (!strcmp(argv[i], "-g") && i + 1 < argc) gate_spec = argv[++i];
        else { usage(argv[0]); return 2; }
    }
    if (!out_path) { usage(argv[0]); return 2; }

    FAUSTDSP *dsp = FAUST_NEW();
    if (!dsp) { fprintf(stderr, "failed to allocate dsp\n"); return 1; }
    FAUST_INIT(dsp, rate);

    ui_t ui;
    build_ui(dsp, &ui);

    /* Apply -p settings. An unknown label is an error, not a warning: silently
     * ignoring it would bake a sample that does not match what was asked for,
     * and you would only notice by ear. */
    for (int i = 0; i < npset; i++) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", pset[i]);
        char *eq = strchr(buf, '=');
        if (!eq) { fprintf(stderr, "bad -p '%s' (want label=value)\n", pset[i]); return 2; }
        *eq = '\0';
        FAUSTFLOAT *z = find_param(&ui, buf);
        if (!z) {
            fprintf(stderr, "no such parameter '%s'. available:\n", buf);
            for (int j = 0; j < ui.n; j++) fprintf(stderr, "  %s\n", ui.params[j].label);
            return 2;
        }
        *z = (FAUSTFLOAT)atof(eq + 1);
    }

    /* Gate envelope: label:on:off, in seconds. */
    FAUSTFLOAT *gate_zone = NULL;
    double gate_on = 0.0, gate_off = 0.0;
    if (gate_spec) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", gate_spec);
        char *c1 = strchr(buf, ':');
        if (!c1) { fprintf(stderr, "bad -g '%s' (want label:on:off)\n", gate_spec); return 2; }
        *c1 = '\0';
        char *c2 = strchr(c1 + 1, ':');
        if (!c2) { fprintf(stderr, "bad -g '%s' (want label:on:off)\n", gate_spec); return 2; }
        *c2 = '\0';
        gate_zone = find_param(&ui, buf);
        if (!gate_zone) { fprintf(stderr, "no such gate parameter '%s'\n", buf); return 2; }
        gate_on = atof(c1 + 1);
        gate_off = atof(c2 + 1);
    }

    const int nouts = FAUST_NUM_OUTPUTS(dsp);
    const int nins = FAUST_NUM_INPUTS(dsp);
    const int channels = (nouts >= 2) ? 2 : 1;
    const int total = (int)(duration * rate);

    /* Faust wants non-interleaved buffers, one pointer per channel. */
    FAUSTFLOAT *obuf[16], *ibuf[16];
    for (int c = 0; c < nouts && c < 16; c++) obuf[c] = calloc(BLOCK, sizeof(FAUSTFLOAT));
    for (int c = 0; c < nins && c < 16; c++) ibuf[c] = calloc(BLOCK, sizeof(FAUSTFLOAT));

    FILE *f = fopen(out_path, "wb");
    if (!f) { perror(out_path); return 1; }
    write_wav_header(f, rate, channels, total);

    for (int pos = 0; pos < total; pos += BLOCK) {
        int n = (total - pos < BLOCK) ? (total - pos) : BLOCK;

        if (gate_zone) {
            double t = (double)pos / rate;
            *gate_zone = (t >= gate_on && t < gate_off) ? (FAUSTFLOAT)1 : (FAUSTFLOAT)0;
        }

        FAUST_COMPUTE(dsp, n, ibuf, obuf);

        for (int i = 0; i < n; i++) {
            for (int c = 0; c < channels; c++) {
                double v = (double)obuf[(c < nouts) ? c : 0][i];
                if (v > 1.0) v = 1.0; else if (v < -1.0) v = -1.0;
                int16_t s = (int16_t)lrint(v * 32767.0);
                put_u16(f, (uint16_t)s);
            }
        }
    }

    fclose(f);
    FAUST_DELETE(dsp);
    fprintf(stderr, "rendered %d frames @ %d Hz, %d ch -> %s\n",
            total, rate, channels, out_path);
    return 0;
}
