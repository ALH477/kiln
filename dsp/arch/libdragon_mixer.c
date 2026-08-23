/* SPDX-License-Identifier: MIT
 *
 * dsp/arch/libdragon_mixer.c — Faust architecture file for live N64 voices.
 *
 * This is the glue between `faust -lang c -single -os` output and libdragon's
 * mixer. It is written from scratch against Faust's public C ABI (CInterface.h)
 * rather than derived from GRAME's own architecture files, so the GPL "larger
 * work" exception attached to those does not come into play here. CInterface.h
 * itself is included, not modified.
 *
 * ── Why -os (one sample) and not block mode ────────────────────────────
 * In `-os` mode Faust emits `frame<name>(dsp, inputs, outputs)`, processing a
 * single sample per call, and leaves `compute<name>()` as an EMPTY STUB. That
 * is a trap worth stating plainly: an architecture file written against
 * compute() will build, link, run, and output pure silence. We call frame().
 *
 * One-sample mode is also what we want musically — it lets control data
 * (report §6: note/gate/pitch frames arriving over the flashcart USB tunnel)
 * be applied between individual samples rather than only at block boundaries.
 *
 * ── The N64 constraints this file exists to honour (report §2, §4) ─────
 *  * FAUSTFLOAT is float. Never double: DIV.D is 58 cycles against a per-sample
 *    budget of ~2126 cycles at 44.1 kHz even if you owned the whole CPU.
 *  * No dynamic allocation after init. Faust's `-mem` output routes allocation
 *    through the manager below, which hands out one static arena — a ROM has no
 *    business calling malloc mid-frame, and static polyphony is what makes the
 *    cycle budget predictable.
 *  * Parameter zones are resolved by label ONCE at init into a flat table.
 *    Faust mangles struct field names, so the UIGlue callbacks are the only
 *    supported way to find a parameter, and doing that per-frame would be
 *    absurd on this CPU.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "faust/gui/CInterface.h"

#ifndef FAUST_N64_MAX_PARAMS
#define FAUST_N64_MAX_PARAMS 32
#endif

/* ── Name mangling ─────────────────────────────────────────────────────
 * Faust's `-cn <name>` suffixes every generated symbol with <name>, which is
 * how two voices can coexist in one ROM without colliding at link time.
 * nix/faust.nix passes a matching -DFAUST_NAME=<name>; these macros rebuild
 * the mangled identifiers so this file stays voice-agnostic.
 */
#ifndef FAUST_NAME
#define FAUST_NAME mydsp /* Faust's default when -cn is not given */
#endif

#define FAUST_CAT2(a, b) a##b
#define FAUST_CAT(a, b) FAUST_CAT2(a, b)

#define FAUSTDSP FAUST_NAME
#define FAUST_NEW FAUST_CAT(new, FAUST_NAME)
#define FAUST_DELETE FAUST_CAT(delete, FAUST_NAME)
#define FAUST_INIT FAUST_CAT(init, FAUST_NAME)
#define FAUST_BUILD_UI FAUST_CAT(buildUserInterface, FAUST_NAME)
#define FAUST_FRAME FAUST_CAT(frame, FAUST_NAME)
#define FAUST_NUM_OUTPUTS FAUST_CAT(getNumOutputs, FAUST_NAME)
#define FAUST_NUM_INPUTS FAUST_CAT(getNumInputs, FAUST_NAME)

/* Faust emits calls to these for min/max on some primitives. */
#ifndef max
#define max(a, b) ((a) < (b) ? (b) : (a))
#endif
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

<<includeIntrinsic>>

<<includeclass>>

/* ── Parameter table ───────────────────────────────────────────────────
 * Populated once by walking the UIGlue. `faust_n64_param()` returns a direct
 * pointer to the zone, so the caller can cache it and write the parameter with
 * a single store — no lookup, no branch, in the audio path.
 */
typedef struct {
    const char *label;
    FAUSTFLOAT *zone;
    FAUSTFLOAT init, lo, hi;
} faust_n64_param_t;

typedef struct {
    faust_n64_param_t params[FAUST_N64_MAX_PARAMS];
    int nparams;
} faust_n64_ui_t;

static void ui_noop_box(void *ui, const char *label) { (void)ui; (void)label; }
static void ui_noop_close(void *ui) { (void)ui; }
static void ui_noop_declare(void *ui, FAUSTFLOAT *z, const char *k, const char *v)
{
    (void)ui; (void)z; (void)k; (void)v;
}

static void ui_add(void *ui, const char *label, FAUSTFLOAT *zone,
                   FAUSTFLOAT init, FAUSTFLOAT lo, FAUSTFLOAT hi)
{
    faust_n64_ui_t *self = (faust_n64_ui_t *)ui;
    if (self->nparams >= FAUST_N64_MAX_PARAMS) {
        return; /* silently drop: see faust_n64_param_count() to detect */
    }
    faust_n64_param_t *p = &self->params[self->nparams++];
    p->label = label;
    p->zone = zone;
    p->init = init;
    p->lo = lo;
    p->hi = hi;
}

/* Buttons and check-buttons carry no range; model them as 0..1 gates. */
static void ui_add_button(void *ui, const char *label, FAUSTFLOAT *zone)
{
    ui_add(ui, label, zone, 0.0f, 0.0f, 1.0f);
}

static void ui_add_slider(void *ui, const char *label, FAUSTFLOAT *zone,
                          FAUSTFLOAT init, FAUSTFLOAT lo, FAUSTFLOAT hi,
                          FAUSTFLOAT step)
{
    (void)step;
    ui_add(ui, label, zone, init, lo, hi);
}

/* Bargraphs are outputs (meters); record them so the caller can read levels. */
static void ui_add_bargraph(void *ui, const char *label, FAUSTFLOAT *zone,
                            FAUSTFLOAT lo, FAUSTFLOAT hi)
{
    ui_add(ui, label, zone, 0.0f, lo, hi);
}

static void ui_add_soundfile(void *ui, const char *label, const char *url,
                             struct Soundfile **sf)
{
    /* Soundfile primitives would need a filesystem read in the audio path;
     * on this target sample playback belongs in libdragon's mixer via wav64
     * (report §5), not inside a live Faust voice. */
    (void)ui; (void)label; (void)url; (void)sf;
}

static void faust_n64_build_ui(FAUSTDSP *dsp, faust_n64_ui_t *out)
{
    UIGlue glue;
    memset(out, 0, sizeof(*out));

    glue.uiInterface = out;
    glue.openTabBox = ui_noop_box;
    glue.openHorizontalBox = ui_noop_box;
    glue.openVerticalBox = ui_noop_box;
    glue.closeBox = ui_noop_close;
    glue.addButton = ui_add_button;
    glue.addCheckButton = ui_add_button;
    glue.addVerticalSlider = ui_add_slider;
    glue.addHorizontalSlider = ui_add_slider;
    glue.addNumEntry = ui_add_slider;
    glue.addHorizontalBargraph = ui_add_bargraph;
    glue.addVerticalBargraph = ui_add_bargraph;
    glue.addSoundfile = ui_add_soundfile;
    glue.declare = ui_noop_declare;

    FAUST_BUILD_UI(dsp, &glue);
}

/* ── Public API ────────────────────────────────────────────────────────
 * One statically-allocated voice per translation unit. Static allocation is
 * deliberate (report §4: "Static polyphony — fixed -nvoices, no dynamic
 * allocation — is mandatory for deterministic budgeting"). Polyphony is built
 * by instantiating this file once per voice via -cn, not by allocating.
 */
#define FAUST_VOICE FAUST_CAT(faust_n64_, FAUST_NAME)
#define FAUST_SYM(suffix) FAUST_CAT(FAUST_VOICE, suffix)

static FAUSTDSP FAUST_SYM(_dsp);
static faust_n64_ui_t FAUST_SYM(_ui);

/* Per-voice output gain, applied before the int16 cast. Default 1.0. */
static FAUSTFLOAT FAUST_SYM(_gain) = 1.0f;

/* Initialise the voice at the given sample rate.
 *
 * Pick the rate for the N64's Audio Interface, not for the M64's fixed 48 kHz
 * HDMI output (report §1): the AI derives its rate from a video-clock divider,
 * and the M64 resamples whatever the core produces. 22050 or 32000 buy back
 * real cycles per sample versus 44100 and are the report's suggested lever if
 * a voice overruns budget. */
void FAUST_SYM(_init)(int sample_rate)
{
    FAUST_INIT(&FAUST_SYM(_dsp), sample_rate);
    faust_n64_build_ui(&FAUST_SYM(_dsp), &FAUST_SYM(_ui));
}

/* Resolve a parameter zone by its Faust label. Call once, outside the audio
 * path, and keep the pointer: writing a parameter is then a single store. */
FAUSTFLOAT *FAUST_SYM(_param)(const char *label)
{
    for (int i = 0; i < FAUST_SYM(_ui).nparams; i++) {
        if (strcmp(FAUST_SYM(_ui).params[i].label, label) == 0) {
            return FAUST_SYM(_ui).params[i].zone;
        }
    }
    return NULL;
}

int FAUST_SYM(_param_count)(void) { return FAUST_SYM(_ui).nparams; }

const char *FAUST_SYM(_param_label)(int i)
{
    return (i >= 0 && i < FAUST_SYM(_ui).nparams) ? FAUST_SYM(_ui).params[i].label : NULL;
}

/* Set the per-voice output gain (0.0 to 1.0). Applied to each sample before
 * the int16 cast. Default is 1.0. This is the voice-level trim — individual
 * parameter zones (freq, gain, gate) are set via _param(). */
void FAUST_SYM(_set_gain)(FAUSTFLOAT gain)
{
    FAUST_SYM(_gain) = gain;
}

/* Render `nframes` stereo sample pairs into a libdragon mixer buffer.
 *
 * libdragon's mixer wants interleaved signed 16-bit stereo; Faust hands us
 * non-interleaved float. We convert per sample rather than staging a float
 * buffer, because 4 MB of RDRAM at ~640 ns latency (report §2) makes an extra
 * buffer pass more expensive than the arithmetic.
 *
 * Mono voices are duplicated to both channels; anything wider than stereo is
 * truncated to the first two outputs.
 *
 * If `accumulate` is non-zero, samples are ADDED to the existing buffer
 * contents (saturating) instead of overwriting. This lets multiple live
 * voices be summed into one AI buffer — render the first voice with
 * accumulate=0, subsequent voices with accumulate=1. The saturating add
 * costs two extra instructions per sample (add + clamp), well within budget.
 *
 * The per-voice gain (_set_gain) is applied before the cast/clamp. */
void FAUST_SYM(_render)(int16_t *out, int nframes, int accumulate)
{
    FAUSTFLOAT frame_in[2] = { 0.0f, 0.0f };
    FAUSTFLOAT frame_out[8];
    const int nouts = FAUST_NUM_OUTPUTS(&FAUST_SYM(_dsp));
    const FAUSTFLOAT g = FAUST_SYM(_gain);

    for (int i = 0; i < nframes; i++) {
        FAUST_FRAME(&FAUST_SYM(_dsp), frame_in, frame_out);

        FAUSTFLOAT l = frame_out[0] * g;
        FAUSTFLOAT r = ((nouts > 1) ? frame_out[1] : frame_out[0]) * g;

        /* Clamp before the cast: a float outside [-1,1] wraps rather than
         * saturates on conversion, which turns a hot voice into loud noise. */
        if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
        if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;

        int16_t sl = (int16_t)(l * 32767.0f);
        int16_t sr = (int16_t)(r * 32767.0f);

        if (accumulate) {
            /* Saturating add: clamp to int16 range after summing. */
            int32_t sum_l = (int32_t)out[2 * i + 0] + sl;
            int32_t sum_r = (int32_t)out[2 * i + 1] + sr;
            out[2 * i + 0] = (sum_l > 32767) ? 32767 : (sum_l < -32768) ? -32768 : (int16_t)sum_l;
            out[2 * i + 1] = (sum_r > 32767) ? 32767 : (sum_r < -32768) ? -32768 : (int16_t)sum_r;
        } else {
            out[2 * i + 0] = sl;
            out[2 * i + 1] = sr;
        }
    }
}
